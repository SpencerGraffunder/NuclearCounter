#include <esp_ota_ops.h>
// Optional diagnostics (loop heartbeat + button trace); the opt-in define lives in
// menu.h so menu.cpp sees it too. Off by default — see the note there.
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
//
// GOTCHA: the hook must be extern "C". It is declared and defined in the core's
// C file (cores/esp32/esp32-hal-misc.c) as a weak C symbol; a plain C++
// definition here gets name-mangled to _Z18verifyRollbackLaterv, so the core's
// weak version stays in force and verification runs immediately — silently
// disabling the whole deferred-rollback path. Check with
// `riscv32-esp-elf-nm firmware.elf | grep verifyRollbackLater` : it must be `T`
// (strong), not `W` (weak).
static const uint32_t OTA_VERIFY_DELAY_MS = 20000;
extern "C" bool verifyRollbackLater() { return true; }
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
  // USB-CDC writes are blocking by default (tx_timeout_ms = 250 ms in
  // USBCDC::write): if the host has the port open but is not draining the TX
  // FIFO, every Serial.print stalls loop() until it times out. That is what made
  // the board look dead to button presses while plugged into USB, while working
  // fine unplugged (tud_cdc_n_connected() is false then, so write() returns
  // immediately). Timeout 0 makes writes drop instead of block, so no host-side
  // stall can ever starve the main loop.
// Bounded, not zero: 0 would drop frames outright when the TX FIFO is full, which
// can lose a RotorHazard protocol response. 20 ms keeps a stall per write bounded
// (the old default was 250 ms, which starved the main loop ~10x whenever the host
// had the port open but was not draining it) while still flushing normally.
#ifndef SERIAL_TX_TIMEOUT_MS
#define SERIAL_TX_TIMEOUT_MS 20
#endif
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(SERIAL_TX_TIMEOUT_MS);
#endif
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

  // Opt-in diagnostics (see C3_DEBUG_HEARTBEAT in include/menu.h). The heartbeat
  // reports both millis() and the loop iteration count: if the loop stalls (a
  // blocking USB-CDC write), millis() advances without iterations, so a beat whose
  // iteration delta is ~0 while its millis delta is >1000 proves a stall.
  static int traceN = 0;
  static uint32_t lastBeat = 0;
  static uint32_t iterations = 0;
  static uint32_t prevIter = 0, maxIterMs = 0;
  uint32_t now = millis();
  uint32_t dt = now - prevIter;
  if (dt > maxIterMs) maxIterMs = dt;   // a blocking USB-CDC write shows up here
  prevIter = now;
#ifdef C3_DEBUG_HEARTBEAT
  iterations++;
  bool trace = traceN < 10;
  if (trace) { traceN++; Serial.printf("[L%d] top\n", traceN); }
  else if (millis() - lastBeat >= 1000) {
    lastBeat = millis();
    // Raw button pin levels: if a press shows no change here, the pin is being
    // held by something external (USB), not a logic problem.
    Serial.printf("[BEAT] mode=%d page=%d btn P=%d S=%d N=%d heap=%lu iter=%lu maxIterMs=%lu\n",
                  (int)integrated.mode(), menu.debugMenuIndex(),
                  digitalRead(PREVIOUS_BUTTON_PIN), digitalRead(SELECT_BUTTON_PIN),
                  digitalRead(NEXT_BUTTON_PIN), ESP.getFreeHeap(),
                  iterations, maxIterMs);
  }
#endif

  battery.updateBatteryVoltage();
#ifdef C3_DEBUG_HEARTBEAT
  if (trace) Serial.println("  - battery ok");
#endif

  if (battery.lowBattery()) {
    buzzer.startAlarm();
  } else {
    buzzer.stopAlarm();
  }

#ifdef INTEGRATED
  integrated.process();
#ifdef C3_DEBUG_HEARTBEAT
  if (trace) Serial.println("  - process ok");
#endif
#endif

  menu.handleButtons();
#ifdef C3_DEBUG_HEARTBEAT
  if (trace) Serial.println("  - buttons ok");
#endif

  menu.clearBuffer();
  menu.drawMenu();
#ifdef C3_DEBUG_HEARTBEAT
  if (trace) Serial.println("  - draw ok");
#endif

  // Draw battery voltage
  menu.drawBatteryVoltage(battery.currentVoltage.get());

  // Send display buffer
  menu.sendBuffer();
}
