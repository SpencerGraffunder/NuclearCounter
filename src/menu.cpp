#include "menu.h"
#include "esp_ota_ops.h"
#ifdef INTEGRATED
#include "integrated.h"
#endif

template <typename T>
const T& clamp(const T& value, const T& low, const T& high) {
    return (value < low) ? low : (value > high) ? high : value;
}

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
    menus[menuIndex].menuIndex = (menus[menuIndex].menuIndex + direction + menus[menuIndex].menuItemsLength) % menus[menuIndex].menuItemsLength;

    // Sound buzzer on button press if necessary
    if (settings->buzzer.get()) buzzer->buzz();

    // Delay for button debouncing
    delay(DEBOUNCE_DELAY);
  }

  // Handle pressing and holding SELECT to go back
  if (selectPressed == HIGH) {
    if (selectButtonPressTime == 0) {  // Button just pressed so record time
      selectButtonPressTime = millis();

      // Sound buzzer on button press if necessary
      if (settings->buzzer.get()) buzzer->buzz();
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
            drawStartingSplash();
            if (!integrated->enterTimer()) {
              menuIndex = MAIN;  // AP failed to start
            }
            break;
          case 2: menuIndex = ABOUT; break;  // Go to about menu
        }
        break;
      case SCAN:  // Band toggle on the scanner page
        module->lowband.set(!module->lowband.get());
        break;
      case WIFI:  // Timer status page: no selectable items
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
          case 0: menuIndex = SETTINGS; break;     // Go to settings menu
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
  u8g2.setFont(u8g2_font_7x13B_tf);
  const char *name = "NuclearCounter";
  u8g2.drawStr(xTextCentre(name, 7), 12, name);

  u8g2.setFont(u8g2_font_7x13_tf);
  u8g2.drawStr(xTextCentre(VERSION, 7), 30, VERSION);

  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(xTextCentre(BASED_ON, 5), 46, BASED_ON);
  u8g2.drawStr(xTextCentre(AUTHOR, 5), 56, AUTHOR);
}

#ifdef INTEGRATED
// WiFi timer page: small header (title top-left, AP IP top-right) over the
// lap stats — last lap, best 3 consecutive, best lap — in the small 5x7 font.
void Menu::drawTimerMenu() {
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(1, 10, "WiFi Timer");
  const char *ip = integrated->apIP().c_str();
  u8g2.drawStr(128 - u8g2.getStrWidth(ip) - 1, 10, ip);

  drawTimerLapRow("Last", integrated->lastLapMs(), 26);
  drawTimerLapRow("Best 3", integrated->best3ConsecutiveMs(), 38);
  drawTimerLapRow("Best", integrated->bestLapMs(), 50);
}

// One timer-page stat row: label left, time value right-aligned. A 0 value
// means "no data yet" and renders as a dash placeholder.
void Menu::drawTimerLapRow(const char *label, uint32_t ms, int y) {
  char value[12];
  if (ms == 0) {
    snprintf(value, sizeof(value), "--.-");
  } else {
    snprintf(value, sizeof(value), "%lu.%02lus", ms / 1000, (ms % 1000) / 10);
  }
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(2, y, label);
  u8g2.drawStr(128 - u8g2.getStrWidth(value) - 2, y, value);
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
  mainMenuItems[0] = { "Scan", bitmap_Scan };
  mainMenuItems[1] = { "WiFi Timer", bitmap_Wifi };
  mainMenuItems[2] = { "About", bitmap_About };
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
  advancedMenuItems[0] = { "Settings", bitmap_Settings };
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
