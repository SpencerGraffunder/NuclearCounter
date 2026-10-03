#include "menu.h"
#include "esp_ota_ops.h"
#ifdef INTEGRATED
#include "integrated.h"
#endif

template <typename T>
const T& clamp(const T& value, const T& low, const T& high) {
    return (value < low) ? low : (value > high) ? high : value;
}

// Timer-page control block element indices (defined early: handleButtons
// needs TP_CTRL_START before the drawing code below defines the layout).
#ifdef INTEGRATED
static const int TP_CTRL_RSSI = 0, TP_CTRL_MINLAP = 1, TP_CTRL_START = 2;
#endif

Menu::Menu(uint8_t p_p, uint8_t s_p, uint8_t n_p, Settings *s, Buzzer *b, RX5808 *r, Api *a)
  : menuIndex(MAIN),
    previous_pin(p_p), select_pin(s_p), next_pin(n_p),
    selectButtonPressTime(0), selectButtonHeld(false),
    settings(s), buzzer(b), module(r), api(a),
    u8g2(U8G2_R0, U8X8_PIN_NONE) {
}

#ifdef INTEGRATED
Menu::Menu(uint8_t p_p, uint8_t s_p, uint8_t n_p, Settings *s, Buzzer *b, RX5808 *r, IntegratedMode *i)
  : menuIndex(MAIN),
    previous_pin(p_p), select_pin(s_p), next_pin(n_p),
    selectButtonPressTime(0), selectButtonHeld(false),
    settings(s), buzzer(b), module(r), api(nullptr),
    integrated(i),
    u8g2(U8G2_R0, U8X8_PIN_NONE) {
}
#endif

// Begin menu object
void Menu::begin() {
  initMenus();

  // Can't call in constructor as pulldown overwritten during boot before setup() called
  pinMode(previous_pin, INPUT_PULLDOWN);
  pinMode(select_pin, INPUT_PULLDOWN);
  pinMode(next_pin, INPUT_PULLDOWN);

  u8g2.begin();
  u8g2.clearBuffer();

#ifdef SH1306
  // SH1306 panels have a per-panel glass defect: two columns near the right
  // edge bleed/glow when the column next to them is driven (visible in every
  // menu, invisible on a pure black frame). The defect location varies per
  // panel, so the app keeps a 6px black guard on the right: nothing is ever
  // drawn into image columns 122-127. On top of that:
  //  1. x_offset = 0 (native SH1306 layout) leaves RAM 128-131 unaddressed.
  //  2. Clear the full RAM twice (three clears at offsets 0/2/4 per pass) so
  //     the unaddressed columns show black, not uninitialized white (the
  //     repeat guards against an I2C flake at boot).
  for (int pass = 0; pass < 2; pass++) {
    u8g2.getU8x8()->x_offset = 0;
    u8g2.clearDisplay();
    u8g2.getU8x8()->x_offset = 2;
    u8g2.clearDisplay();
    u8g2.getU8x8()->x_offset = 4;
    u8g2.clearDisplay();
    u8g2.getU8x8()->x_offset = 0;
    delay(50);
  }
#endif
}

// Handle navigation between menus
// Manipulates the internal menuIndex variable
// Dual boot: request the StarForgeOS app (ota_1 slot, see partitions.csv)
// to boot on the next restart. The bootloader + otadata partition then boot
// the last-selected app, which is also how "boot to last used on power
// cycle" works — no further bookkeeping needed.
static void bootStarForge() {
  const esp_partition_t *starforge = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  if (starforge != nullptr && esp_ota_set_boot_partition(starforge) == ESP_OK) {
    esp_restart();
  }
  // No valid ota_1 slot (e.g. not flashed): fall back to the menu.
}

void Menu::handleButtons() {
  // Check menu button presses
  int prevPressed = digitalRead(previous_pin);
  int selectPressed = digitalRead(select_pin);
  int nextPressed = digitalRead(next_pin);

  // Hidden reset function
  if (prevPressed == HIGH && selectPressed == HIGH && nextPressed == HIGH) {
    settings->clearReset();
  }

  // Update length of scan menu
  menus[SCAN].menuItemsLength = (SCAN_FREQUENCY_RANGE / settings->scanInterval.get()) + 1;  // +1 for final number inclusion

  // Move between menu items
  if (nextPressed == HIGH || prevPressed == HIGH) {
    int direction = (nextPressed == HIGH) ? 1 : -1;
#ifdef INTEGRATED
    if (menuIndex == WIFI) {
      // Timer page: PREV/NEXT drive the control block — move the cursor
      // between the three controls, or inc/dec the value by 2 while editing.
      if (_timerCtrlEditing) {
        timerCtrlAdjust(direction);
      } else {
        _timerCtrlCursor = (_timerCtrlCursor + direction + 3) % 3;
      }
    } else
#endif
    {
      menus[menuIndex].menuIndex = (menus[menuIndex].menuIndex + direction + menus[menuIndex].menuItemsLength) % menus[menuIndex].menuItemsLength;
    }

    // Sound buzzer on button press if necessary
    if (settings->buzzer.get()) buzzer->buzz();

    // Delay for button debouncing
    delay(DEBOUNCE_DELAY);
  }

  // Handle pressing and holding SELECT to go back
  if (selectPressed == HIGH) {
    if (selectButtonPressTime == 0) {  // Button just pressed so record time
      selectButtonPressTime = millis();

      // Sound buzzer on button press if necessary. On the timer page the
      // Start control plays its own race beeps (countdown / Go! / stop)
      // on release, so skip the generic press beep there to avoid a doubled
      // cue; the other two controls rely on this single press beep.
#ifdef INTEGRATED
      bool skipPressBeep = (menuIndex == WIFI && _timerCtrlCursor == TP_CTRL_START);
#else
      bool skipPressBeep = false;
#endif
      if (settings->buzzer.get() && !skipPressBeep) buzzer->buzz();
    } else if (!selectButtonHeld && millis() - selectButtonPressTime > LONG_PRESS_DURATION) {  // Held longer than threshold register long press
#ifdef INTEGRATED
      // Long-press = go back one level toward MAIN. The scanner and WiFi
      // timer pages are temporary overrides of the always-on node, so leaving
      // them must hand the RF hardware back to the node.
      switch (menuIndex) {
        case MAIN: menuIndex = ADVANCED; break;                             // If on main menu, go to advanced
        case SCAN_INTERVAL ... BATTERY_ALARM: menuIndex = SETTINGS; break;  // If on individual settings menu, go to settings
        case SCAN:
          integrated->exitScan();
          menuIndex = MAIN;
          break;
        case WIFI:
          integrated->exitTimer();
          menuIndex = MAIN;
          break;
        case CALIBRATION: menuIndex = ADVANCED; break;                      // If on calibration, go to advanced
        default: menuIndex = MAIN; break;                                   // Otherwise, go back to main menu
      }
#else
      switch (menuIndex) {
        case MAIN: menuIndex = ADVANCED; break;                             // If on main menu, go to advanced
        case SCAN_INTERVAL ... BATTERY_ALARM: menuIndex = SETTINGS; break;  // If on individual settings menu, go to settings
        case WIFI ... CALIBRATION: menuIndex = ADVANCED; break;             // If on individual advanced menu, go to advanced
        default: menuIndex = MAIN; break;                                   // Otherwise, go back to main menu
      }
#endif

      selectButtonHeld = true;

      // Sound double buzz on back if necessary
      if (settings->buzzer.get()) buzzer->doubleBuzz();
    }

    // Delay for button debouncing
    delay(DEBOUNCE_DELAY);

    // Immediately end and wait for next iteration of loop()
    return;
  }

  // If SELECT button was pressed but not held, use as SELECT rather than BACK
  if (selectButtonPressTime > 0 && !selectButtonHeld) {
#ifdef INTEGRATED
    switch (menuIndex) {
      case MAIN:
        switch (menus[MAIN].menuIndex) {
          case 0:  // Scanner page: pause the node (scan task starts next frame)
            integrated->enterScan();
            menuIndex = SCAN;
            break;
          case 1:  // WiFi Timer page: normally instant (AP pre-initialized at
            // boot); the splash only matters on the boot-failure fallback
            // where the slow bring-up runs while it is on screen
            menuIndex = WIFI;
            _timerCtrlCursor = 0;     // start the cursor on the Cross RSSI control
            _timerCtrlEditing = false;
            _timerCtrlEditField = 0;
            drawStartingSplash();
            if (!integrated->enterTimer()) {
              menuIndex = MAIN;  // AP failed to start
            }
            break;
          case 2: menuIndex = SETTINGS; break;  // Go to settings menu
        }
        break;
      case SCAN:  // Band toggle on the scanner page
        module->lowband.set(!module->lowband.get());
        break;
      case WIFI:  // Timer page: Enter acts on the selected control
        timerCtrlSelect();
        break;
      case SETTINGS:  // Handle SELECT on settings menu
        switch (menus[SETTINGS].menuIndex) {
          case 0: menuIndex = SCAN_INTERVAL; break;  // Go to scan interval menu
          case 1: menuIndex = BUZZER; break;         // Go to buzzer menu
          case 2: menuIndex = BATTERY_ALARM; break;  // Go to battery alarm menu
        }
        break;
      case ADVANCED:  // Handle SELECT on advanced menu
        switch (menus[ADVANCED].menuIndex) {
          case 0: menuIndex = ABOUT; break;        // Go to about menu
          case 1: menuIndex = CALIBRATION; break;  // Go to calibration menu
        }
        break;
      case SCAN_INTERVAL ... BATTERY_ALARM:  // Handle SELECT on individual settings options
        switch (menuIndex) {
          case SCAN_INTERVAL:  // Update scan interval settings and icons
            settings->scanIntervalIndex.set(menus[menuIndex].menuIndex);
            menus[SCAN].menuIndex = 0;
            break;
          case BUZZER:  // Update buzzer settings and icons
            settings->buzzerIndex.set(menus[menuIndex].menuIndex);
            break;
          case BATTERY_ALARM:  // Update battery alarm settings and icons
            settings->batteryAlarmIndex.set(menus[menuIndex].menuIndex);
            break;
        }
        break;
      case CALIBRATION:  // Pause the timing engine around the calibration
        switch (menus[CALIBRATION].menuIndex) {
          case 0: calibrateWithTiming(true); break;   // Calibrate high rssi
          case 1: calibrateWithTiming(false); break;  // Calibrate low rssi
        }
        break;
    }
#else
    switch (menuIndex) {
      case MAIN:  // Handle SELECT on main menu
        switch (menus[MAIN].menuIndex) {
          case 0: menuIndex = SCAN; break;      // Go to scan menu
          case 1: menuIndex = SETTINGS; break;  // Go to settings menu
          case 2: menuIndex = ABOUT; break;     // Go to about menu
        }
        break;
      case SCAN:  // Handle SELECT on scan menu
        module->lowband.set(!module->lowband.get());
        break;
      case SETTINGS:  // Handle SELECT on settings menu
        switch (menus[SETTINGS].menuIndex) {
          case 0: menuIndex = SCAN_INTERVAL; break;  // Go to scan interval menu
          case 1: menuIndex = BUZZER; break;         // Go to buzzer menu
          case 2: menuIndex = BATTERY_ALARM; break;  // Go to battery alarm menu
        }
        break;
      case ADVANCED:  // Handle SELECT on advanced menu
        switch (menus[ADVANCED].menuIndex) {
          case 0: menuIndex = WIFI; break;         // Go to Wi-Fi menu
          case 1: menuIndex = CALIBRATION; break;  // Go to calibration menu
          case 2: bootStarForge(); break;          // Boot the StarForgeOS slot
        }
        break;
      case SCAN_INTERVAL ... BATTERY_ALARM:  // Handle SELECT on individual settings options
        switch (menuIndex) {
          case SCAN_INTERVAL:  // Update scan interval settings and icons
            settings->scanIntervalIndex.set(menus[menuIndex].menuIndex);
            menus[SCAN].menuIndex = 0;
            break;
          case BUZZER:  // Update buzzer settings and icons
            settings->buzzerIndex.set(menus[menuIndex].menuIndex);
            break;
          case BATTERY_ALARM:  // Update battery alarm settings and icons
            settings->batteryAlarmIndex.set(menus[menuIndex].menuIndex);
            break;
        }
        break;
      case CALIBRATION:  // Handle SELECT on calibration menu
        switch (menus[CALIBRATION].menuIndex) {
          case 0: module->calibrate(true); break;   // Calibrate high rssi
          case 1: module->calibrate(false); break;  // Calibrate low rssi
        }
        break;
    }
#endif
  }

  // Reset SELECT when button released
  selectButtonPressTime = 0;
  selectButtonHeld = false;
}

// Clear display buffer
void Menu::clearBuffer() {
  u8g2.clearBuffer();
}

// Send data to display buffer
void Menu::sendBuffer() {
  // Only push to the OLED when the frame actually changed. A full 128x64 I2C
  // frame takes ~30ms, which throttled the UI loop to ~27 iters/s and made
  // navigation feel sluggish. Most menu frames are static between button
  // presses (and the timer/scan values only change when they change), so this
  // lets the loop run at full speed whenever the display content is unchanged.
  static uint8_t lastFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT / 8];
  static bool firstFrame = true;
  const uint8_t *buf = (const uint8_t *)u8g2.getBufferPtr();
  uint32_t size = u8g2.getBufferTileWidth() * u8g2.getBufferTileHeight() * 8;
  if (size > sizeof(lastFrame)) {
    size = sizeof(lastFrame);  // safety clamp
  }
  if (!firstFrame && memcmp(buf, lastFrame, size) == 0) {
    return;  // unchanged - skip the I2C push
  }
  memcpy(lastFrame, buf, size);
  firstFrame = false;
  u8g2.sendBuffer();
}

// Draw current menu
void Menu::drawMenu() {
  // Draw title, but not for scan menu or about (the about screen draws its
  // own "NuclearCounter" header at the same position — drawing the generic
  // title too made the two texts overlap)
  bool noGenericTitle = (menuIndex == SCAN || menuIndex == ABOUT);
#ifdef INTEGRATED
  // The WiFi timer page draws its own compact header (title top-left, IP
  // top-right) instead of the big centred title
  noGenericTitle = noGenericTitle || (menuIndex == WIFI);
#endif
  if (!noGenericTitle) {
    u8g2.setFont(u8g2_font_8x13B_tf);
    const char *title = menus[menuIndex].title;
    u8g2.drawStr(xTextCentre(title, 8), 13, title);
    u8g2.setFont(u8g2_font_7x13_tf);
  }

  // Update in-memory icons for individual settings options
  if (menuIndex >= SCAN_INTERVAL && menuIndex <= BATTERY_ALARM) {
    updateSettingsOptionIcons(&menus[SCAN_INTERVAL], settings->scanIntervalIndex.get());
    updateSettingsOptionIcons(&menus[BUZZER], settings->buzzerIndex.get());
    updateSettingsOptionIcons(&menus[BATTERY_ALARM], settings->batteryAlarmIndex.get());
  }

  // Call appropriate draw function
#ifdef INTEGRATED
  switch (menuIndex) {
    case SCAN:  // Scanner bar graph (node paused for the duration)
      module->startScan();
      drawScanMenu();
      break;
    case WIFI:  // WiFi timer status page
      module->stopScan();
      drawTimerMenu();
      break;
    case ABOUT:  // Draw about menu
      drawAboutMenu();
      break;
    default:  // Draw selection menu with options
      module->stopScan();
      drawSelectionMenu();
      break;
  }
#else
  switch (menuIndex) {
    case SCAN:  // Draw scan menu
      module->startScan();
      drawScanMenu();
      break;
    case ABOUT:  // Draw about menu
      drawAboutMenu();
      break;
    case WIFI:  // Draw Wi-Fi menu
      module->startScan();
      api->startWifi();
      drawWifiMenu();
      break;
    default:  // Draw selection menu with options
      module->stopScan();
      api->stopWifi();
      drawSelectionMenu();
      break;
  }
#endif
}

// Display battery voltage in bottom corner of main menu
void Menu::drawBatteryVoltage(int voltage) {
  // Draw voltage only display if on main menu
  if (menuIndex == MAIN) {
    // Format voltage reading
    char formattedVoltage[5];
    snprintf(formattedVoltage, sizeof(formattedVoltage), "%d.%dv", voltage / 10, voltage % 10);

    // Set font colour to inverted if selected bottom item
    u8g2.setDrawColor(menus[MAIN].menuIndex == 2 ? 0 : 1);
    u8g2.setFont(u8g2_font_5x7_tf);
#ifdef SH1306
    // Right-anchored so "x.xv" (4 chars, 6px advance) ends at col 121,
    // inside the 6px black guard (see Menu::begin)
    u8g2.drawStr(99, DISPLAY_HEIGHT, formattedVoltage);
#else
    u8g2.drawStr(109, DISPLAY_HEIGHT, formattedVoltage);
#endif
    u8g2.setDrawColor(1);
  }
}

// Generic function for drawing menus with multiple options
void Menu::drawSelectionMenu() {
  // Draw menu items
  for (int i = 0; i < menus[menuIndex].menuItemsLength; i++) {
    if (i == menus[menuIndex].menuIndex) {
      // Highlight selection. SH1306: inset 6px on the right so the box never
      // reaches the black guard zone (image cols 122-127, see Menu::begin)
#ifdef SH1306
      u8g2.drawBox(0, 16 + (i * 16), DISPLAY_WIDTH - 6, 16);
#else
      u8g2.drawBox(0, 16 + (i * 16), DISPLAY_WIDTH, 16);
#endif
      u8g2.setDrawColor(0);
      u8g2.drawXBMP(10, 17 + (i * 16), 14, 14, menus[menuIndex].menuItems[i].icon);
      u8g2.drawStr(30, 28 + (i * 16), menus[menuIndex].menuItems[i].name);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawXBMP(10, 17 + (i * 16), 14, 14, menus[menuIndex].menuItems[i].icon);
      u8g2.drawStr(30, 28 + (i * 16), menus[menuIndex].menuItems[i].name);
    }
  }

  // Draw extra text for calibration menu
  if (menuIndex == CALIBRATION) {
    u8g2.setFont(u8g2_font_5x7_tf);
    const char *text = "Set to 5800MHz (F4)";
    u8g2.drawStr(xTextCentre(text, 5), 60, text);
  }
}

// Draw graph of scanned rssi values
void Menu::drawScanMenu() {
  // Calculate number of scanned values based off of interval
  int interval = settings->scanInterval.get();
  int numScannedValues = (SCAN_FREQUENCY_RANGE / interval) + 1;  // +1 for final number inclusion

  // Calculate width of each bar in graph by expanding until best fit
  int barWidth = 1;
  while ((barWidth + 1) * numScannedValues <= DISPLAY_WIDTH) {
    barWidth++;
  }

  // Calculate side padding offset for graph
  int padding = (DISPLAY_WIDTH - (barWidth * numScannedValues)) / 2;

  // Get min and max calibrated rssi
  int minRssi = settings->lowCalibratedRssi.get();
  int maxRssi = settings->highCalibratedRssi.get();

  // Draw bottom numbers
  // SH1306: 2px left margin + right label kept inside the 6px right guard
  // (see Menu::begin); other panels use the original positions
#ifdef SH1306
  const int margin = 2;
  const int rightScaleX = 99;
#else
  const int margin = 0;
  const int rightScaleX = 109;
#endif

  u8g2.setFont(u8g2_font_5x7_tf);
  if (module->lowband.get()) {
    u8g2.drawStr(margin, DISPLAY_HEIGHT, "5345");
    u8g2.drawStr(55, DISPLAY_HEIGHT, "5495");
    u8g2.drawStr(rightScaleX, DISPLAY_HEIGHT, "5645");
  } else {
    u8g2.drawStr(margin, DISPLAY_HEIGHT, "5645");
    u8g2.drawStr(55, DISPLAY_HEIGHT, "5795");
    u8g2.drawStr(rightScaleX, DISPLAY_HEIGHT, "5945");
  }

  // Draw high or low band
  u8g2.setFont(u8g2_font_7x13_tf);
  if (module->lowband.get()) {
    u8g2.drawStr(margin, 13, "LOW");
  } else {
    u8g2.drawStr(margin, 13, "HIGH");
  }

  // Draw selected frequency
  char currentFrequency[8];
  int min_freq = module->lowband.get() ? LOWBAND_MIN_FREQUENCY : HIGHBAND_MIN_FREQUENCY;
  snprintf(currentFrequency, sizeof(currentFrequency), "%dMHz", menus[SCAN].menuIndex * interval + min_freq);
  u8g2.drawStr(xTextCentre(currentFrequency, 7), 13, currentFrequency);

  // Safely get current rssi
  int currentFrequencyRssi;
  if (xSemaphoreTake(module->scanMutex, portMAX_DELAY)) {
    currentFrequencyRssi = module->rssiValues.get(menus[SCAN].menuIndex);

    xSemaphoreGive(module->scanMutex);
  }

  // Clamp and convert rssi to percentage
  currentFrequencyRssi = clamp(currentFrequencyRssi, minRssi, maxRssi);
  char percentageStr[5];
  snprintf(percentageStr, sizeof(percentageStr), "%d%%", map(currentFrequencyRssi, minRssi, maxRssi, 0, 100));

  // Draw rssi percentage accounting for changes from 3 to 4 characters
#ifdef SH1306
  // Right-anchored inside the 6px guard: 7x13 font is 7px wide, 8px advance,
  // so the last pixel lands at x + (len-1)*8 + 6 <= 121
  int percentageX = DISPLAY_WIDTH - 7 - (strlen(percentageStr) - 1) * 8 - 6;
#else
  int percentageX = DISPLAY_WIDTH - (strlen(percentageStr) * 7) + 1;
#endif
  u8g2.drawStr(percentageX, 13, percentageStr);

  // Iterate through rssi values
  for (int i = 0; i < numScannedValues; i++) {
    // Safely get current rssi
    int rssi;
    if (xSemaphoreTake(module->scanMutex, portMAX_DELAY)) {
      rssi = module->rssiValues.get(i);

      xSemaphoreGive(module->scanMutex);
    }

    // Clamp rssi between calibrated values
    rssi = clamp(rssi, minRssi, maxRssi);

    // Calculate height of individual bar
    int barHeight = map(rssi, minRssi, maxRssi, 0, BAR_Y_MAX - BAR_Y_MIN);

    // Draw box with x-offset
    // Highlight selection
    if (i == menus[SCAN].menuIndex) {
      u8g2.drawBox(i * barWidth + padding, BAR_Y_MIN, barWidth, BAR_Y_MAX - BAR_Y_MIN);
      u8g2.setDrawColor(0);
      u8g2.drawBox(i * barWidth + padding, BAR_Y_MAX - barHeight, barWidth, barHeight);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawBox(i * barWidth + padding, BAR_Y_MAX - barHeight, barWidth, barHeight);
    }
  }
}

// Draw static content on about menu
void Menu::drawAboutMenu() {
  // 5 lines, sized to fit 128x64 (the two credit lines are 30/26 chars, so
  // they need the small 4x6 font to fit on one line each).
  u8g2.setFont(u8g2_font_7x13B_tf);
  u8g2.drawStr(xTextCentre(APP_NAME, 7), 13, APP_NAME);

  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(xTextCentre(APP_BY, 5), 25, APP_BY);
  u8g2.drawStr(xTextCentre(VERSION, 5), 37, VERSION);

  u8g2.setFont(u8g2_font_4x6_tf);
  u8g2.drawStr(xTextCentre(CREDIT_1, 4), 50, CREDIT_1);
  u8g2.drawStr(xTextCentre(CREDIT_2, 4), 61, CREDIT_2);
}

#ifdef INTEGRATED
// ---- WiFi timer page (OLED race-session control) ----
// SH1306 panels need safe margins: a ~2px left glass offset and a 6px right
// guard (image cols 122-127 are never driven). These anchors keep every glyph
// well inside the safe area:
//   LX  left margin        LVX  left-column value right edge
//   RCX right-col label    RX   right-column value right edge
static const int TP_LX = 4, TP_LVX = 54, TP_RCX = 60, TP_RX = 120;
// Row y-positions are 5x7 drawStr BASELINES (small 5x7 font, 10px apart). The
// top four rows hold the stats; the bottom two rows form the control block
// (label row + value row).
//
// GOTCHA (5x7 ascent, top clipping): u8g2_font_5x7 has ascent_A = 6, so a
// drawStr at baseline y puts the top row of a cap/digit at y-6. Any baseline
// below 6 therefore draws the top of the text off the top edge of the panel
// (u8g2 clips to row 0), so the header looks cut off. The topmost baseline
// here is 7 (glyph top at row 1) for that reason — do not move it back below
// 7. The control block still fits: its highlight box ends at row 58 < 64.
static const int TP_ROW0_Y = 7, TP_ROW1_Y = 17, TP_ROW2_Y = 27, TP_ROW3_Y = 37;
static const int TP_CTRL_LABEL_Y = 47, TP_CTRL_VALUE_Y = 57;
// Control block: three equal elements, each DISPLAY_WIDTH/3 px wide (user
// spec), with label + value text centered inside the element and the
// selection highlight spanning the whole element. Integer division leaves 2px
// uncovered on the right (128 = 42*3 + 2).
// NOTE: the third element's box (cols 84-125) deliberately reaches into the
// old 6px right black-guard zone (cols 122-127, see Menu::begin) — the same
// trade-off the main menu's full-width highlight makes. If this panel's glass
// defect bleeds at the box's right edge, cap the box width there rather than
// reintroducing per-element widths.
static const int TP_COL_W = DISPLAY_WIDTH / 3;  // 42px per control element

// 2-decimal lap value ("12.34"), no trailing "s".
static void tpFmtLapMs(uint32_t ms, char *buf, size_t n) {
  if (ms == 0) {
    snprintf(buf, n, "--.--");
  } else {
    snprintf(buf, n, "%lu.%02lu", (unsigned long)(ms / 1000),
             (unsigned long)((ms % 1000) / 10));
  }
}

// Session clock as M:SS (minutes may exceed 60, so a long session never
// overflows the field). No auto-stop, so this keeps counting indefinitely.
static void tpFmtSessionMs(uint32_t ms, char *buf, size_t n) {
  if (ms == 0) {
    snprintf(buf, n, "--:--");
    return;
  }
  uint32_t total_s = ms / 1000;
  snprintf(buf, n, "%lu:%02lu", (unsigned long)(total_s / 60),
           (unsigned long)(total_s % 60));
}

// Draw a segment of `str` (chars [start, start+len)) at (x, y). When
// `highlight` is set, a white box is drawn behind it and the text is black;
// otherwise the text is the normal white-on-black. Returns the next x.
//
// GOTCHA (5x7 ascent): u8g2_font_5x7 has ascent_A = 6, so drawStr(x, y) puts
// the top row of a cap/digit at y-6, NOT at y. A highlight box therefore has to
// start at y-6 (minus a margin) — starting it at y (or y-1) leaves the top 5
// rows of the glyph outside the box, so black-on-white text is drawn
// black-on-black above the box and the top of the value is unreadable. Same
// reason the main menu's selection box (drawSelectionMenu) reserves 16px per
// row and baselines the text 12px below the box top.
static int tpDrawSeg(U8G2 *u, const char *str, int start, int len, int x, int y,
                     bool highlight) {
  if (len <= 0) return x;
  char tmp[16];
  int n = (len < 15) ? len : 15;
  memcpy(tmp, str + start, n);
  tmp[n] = '\0';
  if (highlight) {
    u->drawBox(x, y - 7, len * 5, 10);  // cover rows y-7..y+2 (glyph is y-6..y)
    u->setDrawColor(0);
    u->drawStr(x, y, tmp);          // black text
    u->setDrawColor(1);
  } else {
    u->drawStr(x, y, tmp);          // white text
  }
  return x + len * 5;
}

// Draw one control element (label + value) in the column at `colX` (width
// TP_COL_W), with the text centered inside the column. Highlight states:
//   0 = not selected (normal), 1 = selected, not editing (a white box covers
//       the WHOLE element — full column width, both lines — text black),
//   2 = editing (only the numeric part of the value is highlighted). For
//       state 2, hlStart/hlLen identify which chars of `value` make up the
//       number (e.g. the "10" of "10s").
static void tpDrawControl(U8G2 *u, int colX, const char *label, const char *value,
                          int state, int hlStart, int hlLen) {
  int labelX = colX + (TP_COL_W - u->getStrWidth(label)) / 2;
  int valueX = colX + (TP_COL_W - u->getStrWidth(value)) / 2;
  if (state == 1) {
    // Full-width box from 1px above the label's glyph top to 2px below the
    // value's glyph bottom. See the 5x7-ascent GOTCHA on tpDrawSeg: baselines
    // are 6px below each glyph top.
    int boxTop = TP_CTRL_LABEL_Y - 7;
    int boxBottom = TP_CTRL_VALUE_Y + 2;
    u->drawBox(colX, boxTop, TP_COL_W, boxBottom - boxTop);
    u->setDrawColor(0);
    u->drawStr(labelX, TP_CTRL_LABEL_Y, label);
    u->drawStr(valueX, TP_CTRL_VALUE_Y, value);
    u->setDrawColor(1);
    return;
  }
  if (state == 2) {
    int rest = (int)strlen(value) - hlStart - hlLen;
    int x2 = tpDrawSeg(u, value, 0, hlStart, valueX, TP_CTRL_VALUE_Y, false);
    int x3 = tpDrawSeg(u, value, hlStart, hlLen, x2, TP_CTRL_VALUE_Y, true);
    tpDrawSeg(u, value, hlStart + hlLen, rest, x3, TP_CTRL_VALUE_Y, false);
    u->drawStr(labelX, TP_CTRL_LABEL_Y, label);
    return;
  }
  u->drawStr(labelX, TP_CTRL_LABEL_Y, label);
  u->drawStr(valueX, TP_CTRL_VALUE_Y, value);
}

// Layout (6 rows, small 5x7 font):
//   WiFi Timer          192.168.8.1
//   Last   --.--        Best   --.--
//   Laps   ---          Best 3 --.--
//   Time   --:--/Start  RSSI   62
//   [  RSSI  ][ Min Lap ][  Start  ]   (3 equal elements, each screen/3 wide)
//   [ 145/205 ][   10s   ][  Race   ]   (label + value centered in each)
void Menu::drawTimerMenu() {
  u8g2.setFont(u8g2_font_5x7_tf);
  char v[16];

  // Header: title left, AP IP right. The IP is held in a local String so its
  // c_str() stays valid for the drawStr below (a temporary String's c_str()
  // dangles and the IP would not render).
  u8g2.drawStr(TP_LX, TP_ROW0_Y, "WiFi Timer");
  String ip = integrated->apIP();
  u8g2.drawStr(TP_RX - u8g2.getStrWidth(ip.c_str()), TP_ROW0_Y, ip.c_str());

  // Last | Best
  u8g2.drawStr(TP_LX, TP_ROW1_Y, "Last");
  tpFmtLapMs(integrated->lastLapMs(), v, sizeof(v));
  u8g2.drawStr(TP_LVX - u8g2.getStrWidth(v), TP_ROW1_Y, v);
  u8g2.drawStr(TP_RCX, TP_ROW1_Y, "Best");
  tpFmtLapMs(integrated->bestLapMs(), v, sizeof(v));
  u8g2.drawStr(TP_RX - u8g2.getStrWidth(v), TP_ROW1_Y, v);

  // Laps | Best 3
  u8g2.drawStr(TP_LX, TP_ROW2_Y, "Laps");
  int laps = integrated->sessionLapCount();
  if (laps <= 0) {
    snprintf(v, sizeof(v), "---");
  } else {
    snprintf(v, sizeof(v), "%d", laps);
  }
  u8g2.drawStr(TP_LVX - u8g2.getStrWidth(v), TP_ROW2_Y, v);
  u8g2.drawStr(TP_RCX, TP_ROW2_Y, "Best 3");
  tpFmtLapMs(integrated->best3ConsecutiveMs(), v, sizeof(v));
  u8g2.drawStr(TP_RX - u8g2.getStrWidth(v), TP_ROW2_Y, v);

  // Time | RSSI (live signal). The elapsed-time value box doubles as the
  // countdown display: during the pre-start countdown it shows "Start in N"
  // (which fills the whole left column, so the "Time" label is suppressed),
  // then "Go!" for the first ~1.2s, then the running clock.
  if (integrated->countdownActive()) {
    snprintf(v, sizeof(v), "Start in %d", integrated->countdownRemaining());
    u8g2.drawStr(TP_LVX - u8g2.getStrWidth(v), TP_ROW3_Y, v);
  } else if (integrated->goFlashActive()) {
    u8g2.drawStr(TP_LVX - u8g2.getStrWidth("Go!"), TP_ROW3_Y, "Go!");
  } else {
    u8g2.drawStr(TP_LX, TP_ROW3_Y, "Time");
    tpFmtSessionMs(integrated->sessionElapsedMs(), v, sizeof(v));
    u8g2.drawStr(TP_LVX - u8g2.getStrWidth(v), TP_ROW3_Y, v);
  }

  // Current RSSI (0-255) readout in the right column. Throttled to ~5 Hz so a
  // live signal does not force a full I2C frame push every loop (which would
  // saturate the bus and throttle the UI loop).
  u8g2.drawStr(TP_RCX, TP_ROW3_Y, "RSSI");
  static uint8_t shownRssi = 0;
  static uint32_t nextRssiUpdate = 0;
  if (millis() >= nextRssiUpdate) {
    shownRssi = integrated->rssi();
    nextRssiUpdate = millis() + 200;
  }
  snprintf(v, sizeof(v), "%d", (int)shownRssi);
  u8g2.drawStr(TP_RX - u8g2.getStrWidth(v), TP_ROW3_Y, v);

  // ---- Control block: Cross RSSI | Min Lap | Start ----
  // Which element is highlighted, and how (1 = both lines, 2 = numeric only).
  int rssiState = 0, minLapState = 0, startState = 0, hlStart = 0, hlLen = 0;
  if (!_timerCtrlEditing) {
    if (_timerCtrlCursor == TP_CTRL_RSSI) rssiState = 1;
    else if (_timerCtrlCursor == TP_CTRL_MINLAP) minLapState = 1;
    else startState = 1;
  } else if (_timerCtrlCursor == TP_CTRL_RSSI) {
    char enter[4], exitv[4];
    snprintf(enter, sizeof(enter), "%d", integrated->getEnterRSSI());
    snprintf(exitv, sizeof(exitv), "%d", integrated->getExitRSSI());
    rssiState = 2;
    if (_timerCtrlEditField == 0) {
      hlStart = 0; hlLen = (int)strlen(enter);      // highlight the enter number
    } else {
      hlStart = (int)strlen(enter) + 1;             // skip "E/"
      hlLen = (int)strlen(exitv);                   // highlight the exit number
    }
  } else if (_timerCtrlCursor == TP_CTRL_MINLAP) {
    char sec[4];
    snprintf(sec, sizeof(sec), "%d", integrated->getMinLapSeconds());
    minLapState = 2;
    hlStart = 0; hlLen = (int)strlen(sec);          // highlight the number (not "s")
  }

  char crossVal[12];
  snprintf(crossVal, sizeof(crossVal), "%d/%d", integrated->getEnterRSSI(),
           integrated->getExitRSSI());
  tpDrawControl(&u8g2, 0, "RSSI", crossVal, rssiState, hlStart, hlLen);

  char minVal[6];
  snprintf(minVal, sizeof(minVal), "%ds", integrated->getMinLapSeconds());
  tpDrawControl(&u8g2, TP_COL_W, "Min Lap", minVal, minLapState, hlStart, hlLen);

  const char *startVal = integrated->raceActive() ? "Stop" : "Race";
  tpDrawControl(&u8g2, 2 * TP_COL_W, "Start", startVal, startState, 0, 0);
}

// ENTER on the timer page: act on the currently selected control.
//   Cross RSSI: enter -> begin editing the enter value; enter -> switch to the
//               exit value; enter -> commit + exit edit.
//   Min Lap:    enter -> toggle edit (second enter commits + exits).
//   Start:      enter -> start/stop the race (its own beeps/countdown).
// On commit the timing settings are persisted to NVS (matching the web UI).
void Menu::timerCtrlSelect() {
  bool commit = false;
  switch (_timerCtrlCursor) {
    case TP_CTRL_RSSI:
      if (!_timerCtrlEditing) {
        _timerCtrlEditing = true;
        _timerCtrlEditField = 0;
      } else if (_timerCtrlEditField == 0) {
        _timerCtrlEditField = 1;
      } else {
        _timerCtrlEditing = false;
        commit = true;
      }
      break;
    case TP_CTRL_MINLAP:
      _timerCtrlEditing = !_timerCtrlEditing;
      commit = !_timerCtrlEditing;
      break;
    case TP_CTRL_START:
      integrated->oledSelectTimer();  // start/stop the race session
      break;
  }
  if (commit) {
    integrated->saveTimingSettings();
  }
  // No beep here: the generic SELECT handler already beeps once on button
  // PRESS (see handleButtons). Beeping again on release made Enter sound like
  // a double beep. The Start control's race beeps (countdown / Go! / stop)
  // come from IntegratedMode::oledSelectTimer, not here.
}

// PREV/NEXT while editing a control: inc (next) / dec (prev) the value by 2.
// Cross RSSI keeps the enter > exit invariant; min lap is clamped 0..60 s.
void Menu::timerCtrlAdjust(int direction) {
  int step = 2 * direction;
  if (_timerCtrlCursor == TP_CTRL_RSSI) {
    if (_timerCtrlEditField == 0) {
      int e = integrated->getEnterRSSI();
      int x = integrated->getExitRSSI();
      e += step;
      if (e > 255) e = 255;
      if (e < x + 2) e = x + 2;  // keep enter strictly above exit
      integrated->setEnterRSSI(e);
    } else {
      int e = integrated->getEnterRSSI();
      int x = integrated->getExitRSSI();
      x += step;
      if (x > e - 2) x = e - 2;  // keep exit strictly below enter
      if (x < 0) x = 0;
      integrated->setExitRSSI(x);
    }
  } else if (_timerCtrlCursor == TP_CTRL_MINLAP) {
    int sec = integrated->getMinLapSeconds();
    sec += step;
    if (sec < 0) sec = 0;
    if (sec > 60) sec = 60;
    integrated->setMinLapSeconds(sec);
  }
}

// Shown immediately when the user selects "WiFi Timer", before the
// multi-second blocking AP bring-up runs, so the screen responds at once
// instead of looking frozen on the main menu.
void Menu::drawStartingSplash() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_7x13B_tf);
  const char *t1 = "WiFi Timer";
  u8g2.drawStr(xTextCentre(t1, 7), 28, t1);
  u8g2.setFont(u8g2_font_7x13_tf);
  const char *t2 = "Starting...";
  u8g2.drawStr(xTextCentre(t2, 7), 44, t2);
  u8g2.sendBuffer();
}

// RX5808::calibrate() bit-bangs the same pins the 1ms timing task drives in
// NODE/TIMER modes, so the engine is paused around the call (and re-tuned
// afterwards, since calibrate() leaves the hardware at 5800 MHz).
void Menu::calibrateWithTiming(bool high) {
  bool wasActive = integrated->pauseForCalibration();
  module->calibrate(high);
  integrated->resumeAfterCalibration(wasActive);
}
#endif

// Draw static content on Wi-Fi menu
void Menu::drawWifiMenu() {
  // Draw SSID
  u8g2.setFont(u8g2_font_7x13B_tf);
  u8g2.drawStr(11, 28, "ID");
  u8g2.setFont(u8g2_font_7x13_tf);
  u8g2.drawStr(30, 28, WIFI_SSID);

  // Draw password
  u8g2.setFont(u8g2_font_7x13B_tf);
  u8g2.drawStr(4, 44, "PWD");
  u8g2.setFont(u8g2_font_7x13_tf);
  u8g2.drawStr(30, 44, WIFI_PASSWORD);

  // Draw IP
  u8g2.setFont(u8g2_font_7x13B_tf);
  u8g2.drawStr(11, 60, "IP");
  if (strlen(WIFI_IP) < 15) {  // If not 15 characters use regular font
    u8g2.setFont(u8g2_font_7x13_tf);
    u8g2.drawStr(30, 60, WIFI_IP);
  } else {  // If 15 characters use smaller font, otherwise last digit off screen
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(30, 59, WIFI_IP);
  }
}

// Update icons for selected settings options
void Menu::updateSettingsOptionIcons(menuStruct *menu, int selectedIndex) {
  for (int i = 0; i < menu->menuItemsLength; i++) {
    if (i == selectedIndex) {
      menu->menuItems[i].icon = bitmap_Selected;
    } else {
      menu->menuItems[i].icon = bitmap_Blank;
    }
  }
}

// Initialise menu structures
void Menu::initMenus() {
#ifdef INTEGRATED
  // NOTE: the display fits at most THREE rows per selection page (16px rows
  // starting at y=16, below the title and above the battery voltage). Do not
  // add more items to any menu — extra rows render off-screen.
  // Main menu: the RotorHazard USB node is ALWAYS ON (it is not a menu item);
  // these rows open the pages that temporarily override it.
  mainMenuItems[0] = { "Scanner", bitmap_Scan };
  mainMenuItems[1] = { "WiFi Timer", bitmap_Wifi };
  mainMenuItems[2] = { "Settings", bitmap_Settings };
#else
  // Main menu
  mainMenuItems[0] = { "Scan", bitmap_Scan };
  mainMenuItems[1] = { "Settings", bitmap_Settings };
  mainMenuItems[2] = { "About", bitmap_About };
#endif

  // Settings menu
  settingsMenuItems[0] = { "Scan interval", bitmap_Interval };
  settingsMenuItems[1] = { "Buzzer", bitmap_Buzzer };
  settingsMenuItems[2] = { "Bat. alarm", bitmap_Alarm };

  // Scan Interval menu
  scanIntervalMenuItems[0] = { "5MHz", bitmap_Blank };
  scanIntervalMenuItems[1] = { "10MHz", bitmap_Blank };
  scanIntervalMenuItems[2] = { "20MHz", bitmap_Blank };

  // Buzzer menu
  buzzerMenuItems[0] = { "On", bitmap_Blank };
  buzzerMenuItems[1] = { "Off", bitmap_Blank };

  // Battery Alarm menu
  batteryAlarmMenuItems[0] = { "3.6v", bitmap_Blank };
  batteryAlarmMenuItems[1] = { "3.3v", bitmap_Blank };
  batteryAlarmMenuItems[2] = { "3.0v", bitmap_Blank };

#ifdef INTEGRATED
  // Advanced menu (integrated): 2 rows (the 3-row display limit is enforced
  // in the comment above)
  advancedMenuItems[0] = { "About", bitmap_About };
  advancedMenuItems[1] = { "Calibration", bitmap_Calibration };
#else
  // Advanced menu
  advancedMenuItems[0] = { "Wi-Fi", bitmap_Wifi };
  advancedMenuItems[1] = { "Calibration", bitmap_Calibration };
  advancedMenuItems[2] = { "StarForge", bitmap_Star };
#endif

  // Calibration menu
  calibrationMenuItems[0] = { "Calib. high", bitmap_Wifi };
  calibrationMenuItems[1] = { "Calib. low", bitmap_WifiLow };

  // Menus
  menus[0] = { "NuclearCounter", mainMenuItems, 3, 0 };
  menus[1] = { "Scan", nullptr, MAX_FREQUENCIES_SCANNED, 0 };
  menus[2] = { "Settings", settingsMenuItems, 3, 0 };
  menus[3] = { "About", nullptr, 1, 0 };
#ifdef INTEGRATED
  menus[4] = { "Advanced", advancedMenuItems, 2, 0 };
#else
  menus[4] = { "Advanced", advancedMenuItems, 3, 0 };
#endif
  menus[5] = { "Scan interval", scanIntervalMenuItems, 3, 0 };
  menus[6] = { "Buzzer", buzzerMenuItems, 2, 0 };
  menus[7] = { "Bat. alarm", batteryAlarmMenuItems, 3, 0 };
#ifdef INTEGRATED
  menus[8] = { "WiFi Timer", nullptr, 1, 0 };
#else
  menus[8] = { "Wi-Fi", nullptr, 1, 0 };
#endif
  menus[9] = { "Calibration", calibrationMenuItems, 2, 0 };
}

// Calculate x position of text to centre it on screen
int Menu::xTextCentre(const char *text, int fontCharWidth) {
  // +1 to include blank space pixel on right edge of final character
  return (DISPLAY_WIDTH - (strlen(text) * fontCharWidth)) / 2 + 1;
}
