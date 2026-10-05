#include <esp_ota_ops.h>
#include "battery.h"
#include "buzzer.h"
#include "menu.h"
#include "RX5808.h"
#include "settings.h"
#ifdef INTEGRATED
#include "integrated.h"
#else
#include "api.h"
#endif

// Create settings object to store settings state
// Initialised with default settings
Settings settings;

// Create buzzer object
Buzzer buzzer(BUZZER_PIN);

// Create battery object
Battery battery(BATTERY_PIN, &settings);

// Create RX5808 object
RX5808 module(SPI_DATA_PIN, SPI_LE_PIN, SPI_CLK_PIN, RSSI_PIN, &settings);

#ifndef INTEGRATED
// Create api object
Api api(&settings, &module, &battery);
#endif

#ifdef INTEGRATED
// Integrated mode orchestrator (SFOS-ported timing / USB node / web stack)
IntegratedMode integrated(&settings, &module, &buzzer);

// ---- Rollback-safe OTA -----------------------------------------------------
// The pre-built framework core AND bootloader are both compiled with
// *_APP_ROLLBACK_ENABLE (see framework-arduinoespressif32-libs/<chip>/
// sdkconfig): after an OTA, the new image boots in PENDING_VERIFY state and
// the bootloader reverts to the previous slot on the NEXT boot unless the
// app calls esp_ota_mark_app_valid_cancel_rollback() during this one.
//
// By default the core marks the image valid immediately in initArduino() —
// BEFORE setup() — so a crash inside setup() would never roll back. Deferring
// with verifyRollbackLater() (a weak hook the core consults at startup) and
// confirming after OTA_VERIFY_DELAY_MS of stable uptime instead covers
// crashes anywhere in boot, including setup() and the first seconds of loop().
static const uint32_t OTA_VERIFY_DELAY_MS = 20000;
bool verifyRollbackLater() { return true; }
#endif

// Create menu object
#ifdef INTEGRATED
Menu menu(PREVIOUS_BUTTON_PIN, SELECT_BUTTON_PIN, NEXT_BUTTON_PIN, &settings, &buzzer, &module, &integrated);
#else
Menu menu(PREVIOUS_BUTTON_PIN, SELECT_BUTTON_PIN, NEXT_BUTTON_PIN, &settings, &buzzer, &module, &api);
#endif

void setup() {
  // Setup serial for debugging
  Serial.begin(115200);
  delay(150);

  // Load settings from non-volatile memory
  settings.loadSettingsStorage();

#ifdef INTEGRATED
  // Start the always-on RotorHazard USB node baseline; the Scan and WiFi
  // Timer pages temporarily override it while they are open
  integrated.begin();
#endif

  // Setup menu
  menu.begin();

  // Double buzz for initialisation complete
  buzzer.doubleBuzz();

  // Allow for serial to connect
  delay(200);
}

void loop() {
#ifdef INTEGRATED
  // Confirm a freshly-OTA'd image once it has stayed up long enough (see
  // verifyRollbackLater above). No-op when the running image is already
  // marked VALID (the normal case), so this runs at most one NVS write per
  // boot, 20s in.
  static bool otaConfirmed = false;
  if (!otaConfirmed && (millis() >= OTA_VERIFY_DELAY_MS)) {
    otaConfirmed = true;
    esp_ota_mark_app_valid_cancel_rollback();
  }
#endif

  // Update battery voltage each loop
  battery.updateBatteryVoltage();

  // Start battery alarm if low voltage
  if (battery.lowBattery()) {
    buzzer.startAlarm();
  } else {
    buzzer.stopAlarm();
  }

#ifdef INTEGRATED
  // Background mode work (RotorHazard node serial protocol, timing)
  integrated.process();
#endif

  // Handle button presses
  // Menu object internally stores which menu currently on
  menu.handleButtons();

  // Clear display buffer
  menu.clearBuffer();

  // Draw menus using internal menu and settings states
  menu.drawMenu();

  // Draw battery voltage
  menu.drawBatteryVoltage(battery.currentVoltage.get());

  // Send display buffer
  menu.sendBuffer();
}
