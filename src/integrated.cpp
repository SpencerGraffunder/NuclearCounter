#ifdef INTEGRATED

#include "integrated.h"
#include "about.h"
#include <WiFi.h>
#include <esp_ota_ops.h>

// Firmware info strings reported over the RotorHazard protocol
// (READ_FIRMWARE_INFO) — externs declared in node_mode.cpp.
const char *firmwareVersionString = VERSION;
const char *firmwareBuildDateString = __DATE__;
const char *firmwareBuildTimeString = __TIME__;
const char *firmwareProcTypeString = "ESP32";

// The TimingCore lap callback is a plain C function pointer, so it needs a
// global handle to reach the single IntegratedMode instance.
static IntegratedMode *s_self = nullptr;

// Lap callback (fires from the timing task on every crossing, in any mode
// where the engine is activated). Feeds the web/OLED lap vector (addLap)
// AND the RotorHazard node's lap counter (nodeLap). The node counter MUST be
// updated here rather than by draining the timing-core ring in NodeMode:
// the ring is only consumed while the node protocol is running, so laps
// recorded while the timer/scanner page was open used to sit in the 50-slot
// ring until the user returned — and >50 of them overflowed the ring and
// froze the node's lap counter for RotorHazard.
static void onTimingLap(const LapData &lap) {
  if (s_self) {
    s_self->addLap(lap);
    s_self->nodeLap(lap);
  }
}

IntegratedMode::IntegratedMode(Settings *settings, RX5808 *rx, Buzzer *buzzer)
  : _settings(settings), _rx(rx), _buzzer(buzzer) {
  s_self = this;
}

void IntegratedMode::begin() {
  Serial.println(F("NuclearCounter integrated firmware (USB node always on, scanner + WiFi timer pages)"));

  // Log which slot we booted from + its verify state. After an OTA the new
  // slot shows state=1 (PENDING_VERIFY) until main.ino confirms it 20s in;
  // if the app crash-loops instead, the next boot reverts to the previous
  // slot (rollback-safe OTA — see main.ino).
  const esp_partition_t *runPart = esp_ota_get_running_partition();
  esp_ota_img_states_t imgState = ESP_OTA_IMG_UNDEFINED;
  esp_ota_get_state_partition(runPart, &imgState);
  Serial.printf("Boot slot: %s (image state: %d, 0=valid 1=pending verify)\n",
                runPart ? runPart->label : "?", (int)imgState);

  // No WiFi at boot: the AP only comes up while the WiFi Timer page is
  // active (enterTimer/exitTimer). Skipping the boot-time AP also keeps
  // RotorHazard node detection fast (the node is ready ~2 s after reset).
  startNode();
}

void IntegratedMode::process() {
  tickRace();  // countdown / go-flash / lap-flash / status line (runs in all modes)
  if (_mode == IntMode::NODE) {
    _node.process();  // includes handleSerialInput()
  } else if (_mode == IntMode::TIMER) {
    // RotorHazard stays connected while the timer page is open: the protocol is
    // served from the live timing-core state, so the server keeps seeing the node,
    // can still set frequency/thresholds, and receives laps from the page-run
    // session. The OLED page reads the same timing-core state, so both views agree.
    _node.handleSerialInput();
  } else {
    // SCANNING: the RotorHazard host is not in control; discard any protocol
    // bytes so stale commands (e.g. WRITE_FREQUENCY) can't steer the scanner.
    // The timing core is deactivated here, so reads would report stale values.
    drainSerial();
  }
  if (_mode != IntMode::SCANNING) {
    _timing.process();  // loop-side timing bookkeeping (SFOS main-loop duty)
  }
}

// (Re)start the always-on node baseline. Idempotent: safe to call after
// returning from the scanner page or the WiFi timer page.
void IntegratedMode::startNode() {
  if (!_timingBegun) {
    _timing.setRX5808(_rx);
    _timing.begin();
    _timing.setLapCallback(onTimingLap);
    _timingBegun = true;
  }
  _node.begin(&_timing);  // first call seeds defaults; later calls re-activate
  _timingSettings.loadSettings(&_timing);  // persisted band/channel/thresholds
  _timing.setActivated(true);
  _mode = IntMode::NODE;
}

// Scanner page entry: pause the node so the scan task owns the RX5808.
// (The menu starts the scan task itself on the next frame.)
void IntegratedMode::enterScan() {
  if (_mode == IntMode::SCANNING) {
    return;
  }
  _timing.setActivated(false);
  _mode = IntMode::SCANNING;
  Serial.println(F("Node paused: scanner page"));
}

// Scanner page exit: stop the scanner first (it may be mid bit-bang), then
// resume the node and force a re-tune — the scan task bit-banged the RF pins
// directly, so the hardware is left on the sweep's last frequency.
void IntegratedMode::exitScan() {
  if (_mode != IntMode::SCANNING) {
    return;
  }
  _rx->stopScan();
  // vTaskDelete takes effect at the scan task's next yield — give it a moment
  // to finish any in-flight bit-bang so our re-tune is the last word on the bus.
  delay(5);
  startNode();
  _timing.setFrequency(_timing.getCurrentFrequency());  // force hardware re-tune
  Serial.println(F("Node resumed after scanner page"));
}

// WiFi timer page entry: full AP + web bring-up (blocking, ~3-4s — the
// menu shows the "Starting..." splash for exactly this window). The radio
// is fully torn down on exit, so every entry re-runs the whole sequence.
bool IntegratedMode::enterTimer() {
  if (_mode == IntMode::TIMER) {
    return true;  // already up (re-press guard)
  }
  if (_mode == IntMode::SCANNING) {
    _rx->stopScan();
    delay(5);  // let the scan task finish any in-flight bit-bang
  }
  if (!_apUp) {
    Serial.println(F("Starting WiFi timer..."));
#ifdef C3_DEBUG_AUTO_TIMER
    Serial.printf("[T] heap before setupAP: %lu\n", ESP.getFreeHeap());
#endif
    if (!_wifi.setupAP()) {
      Serial.println(F("WiFi AP failed to start"));
      startNode();
      return false;
    }
#ifdef C3_DEBUG_AUTO_TIMER
    Serial.println("[T] setupAP returned ok");
#endif
    if (!_webBegun) {
      // One-time: register routes + mount SPIFFS (both survive AP
      // down/up cycles; only the TCP listener needs re-binding)
      _web.begin(&_timing, &_timingSettings, &_raceActive, &_raceStartTime, &_laps);
      _webBegun = true;
#ifdef C3_DEBUG_AUTO_TIMER
      Serial.println("[T] web.begin done");
#endif
    }
    _web.start();  // wait for IP, mDNS, TCP listener
#ifdef C3_DEBUG_AUTO_TIMER
    Serial.println("[T] web.start done");
#endif
    _apUp = true;
  }
  _timing.setActivated(true);
  // Defensive re-tune: if the scanner had driven the RF pins, the hardware
  // is left on the sweep's last frequency.
  _timing.setFrequency(_timing.getCurrentFrequency());
  _mode = IntMode::TIMER;
  Serial.printf("Mode: WiFi timer (%s)\n", WiFi.softAPIP().toString().c_str());
  return true;
}

// WiFi timer page exit: tear the WiFi stack down completely so the AP
// disappears (the radio is only supposed to be on while the timer page is
// active). Teardown ORDER matters on the S3: softAPdisconnect(true) deletes
// the AP interface and WiFi.mode(WIFI_OFF) releases the driver — re-arming
// WiFi.mode(WIFI_AP) while a live AP instance still exists crashes in
// ieee80211_hostap_attach (observed 2026-10-02). Stop the TCP listener
// first so no handler runs while the stack unwinds.
void IntegratedMode::exitTimer() {
  if (_mode != IntMode::TIMER) {
    return;
  }
  if (_apUp) {
    _web.stop();
    WiFi.softAPdisconnect(true);
    delay(100);  // give the WiFi stack time to clean up
    WiFi.mode(WIFI_OFF);
    _apUp = false;
    Serial.println(F("WiFi AP down"));
  }
  startNode();
  Serial.println(F("Node resumed (WiFi off)"));
}

bool IntegratedMode::pauseForCalibration() {
  if (_mode == IntMode::SCANNING || !_timingBegun) {
    return false;  // timing engine not active; nothing to pause
  }
  return _timing.pauseTemporarily(500);
}

void IntegratedMode::resumeAfterCalibration(bool wasActive) {
  if (_mode == IntMode::SCANNING || !_timingBegun) {
    return;
  }
  _timing.resumeFromPause(wasActive);
  // calibrate() tuned the hardware to 5800 MHz; force the timing core to
  // re-tune back to its tracked frequency.
  _timing.setFrequency(_timing.getCurrentFrequency());
}

void IntegratedMode::drainSerial() {
  while (Serial.available() > 0) {
    Serial.read();
  }
}

uint32_t IntegratedMode::lastLapMs() const {
  return _laps.empty() ? 0 : _laps.back().lap_time_ms;
}

uint32_t IntegratedMode::bestLapMs() const {
  uint32_t best = 0;
  for (const auto &lap : _laps) {
    if (lap.valid && lap.lap_time_ms > 0 &&
        (best == 0 || lap.lap_time_ms < best)) {
      best = lap.lap_time_ms;
    }
  }
  return best;
}

// Fastest sum of any 3 consecutive laps (the "best 3 in a row" racing metric).
uint32_t IntegratedMode::best3ConsecutiveMs() const {
  if (_laps.size() < 3) {
    return 0;
  }
  uint32_t best = 0;
  for (size_t i = 0; i + 2 < _laps.size(); i++) {
    if (!(_laps[i].valid && _laps[i + 1].valid && _laps[i + 2].valid)) {
      continue;
    }
    uint32_t sum = _laps[i].lap_time_ms + _laps[i + 1].lap_time_ms +
                   _laps[i + 2].lap_time_ms;
    if (sum > 0 && (best == 0 || sum < best)) {
      best = sum;
    }
  }
  return best;
}

void IntegratedMode::addLap(const LapData &lap) {
  if (!_raceActive) {
    return;  // only record laps for the active session (matches the web UI)
  }
  _laps.push_back(lap);
  while (_laps.size() > 100) {
    _laps.erase(_laps.begin());
  }
  // Lap feedback (works in any menu mode, not just the timer page):
  // a new best-3 (fastest 3 consecutive) beeps DOUBLE, any other lap beeps
  // once. The lap count itself is shown in the grid, not the status line.
  uint32_t b3 = best3ConsecutiveMs();
  bool newBest3 = (b3 > 0) && (_lastBest3Ms == 0 || b3 < _lastBest3Ms);
  if (b3 > 0) {
    _lastBest3Ms = b3;
  }
  if (_buzzer) {
    if (newBest3) {
      _buzzer->doubleBuzz();
    } else {
      _buzzer->buzz();
    }
  }
}

void IntegratedMode::nodeLap(const LapData &lap) {
  _node.onLap(lap);
}

// SELECT on the WiFi timer page: idle -> start (countdown), countdown ->
// cancel, running -> stop. The web can start/stop the same session in parallel
// (it sets _raceActive/_raceStartTime directly); tickRace() reconciles the two.
void IntegratedMode::oledSelectTimer() {
  if (_raceActive) {
    _raceActive = false;  // stop the session; keep _laps so the stats persist
    Serial.println(F("OLED: race stopped"));
  } else if (_countdownActive) {
    _countdownActive = false;  // cancel the running countdown
    Serial.println(F("OLED: countdown cancelled"));
  } else {
    _countdownActive = true;
    _countdownStartMs = millis();
    _countdownLastBeepSecond = 0;  // force a beep on the first second
    Serial.println(F("OLED: race start (countdown)"));
  }
}

uint32_t IntegratedMode::sessionElapsedMs() const {
  if (!_raceActive) {
    return 0;
  }
  // Unsigned subtraction wraps correctly across millis() rollover (~49 days).
  return millis() - _raceStartTime;
}

int IntegratedMode::countdownRemaining() const {
  if (!_countdownActive) {
    return 0;
  }
  int rem = RACE_COUNTDOWN_SECONDS - (int)((millis() - _countdownStartMs) / 1000);
  return (rem < 1) ? 1 : rem;
}

bool IntegratedMode::goFlashActive() const { return millis() < _goFlashUntilMs; }

// Advance the race session state machine and refresh the status line. Called
// from process() every loop, so it runs regardless of which menu page is up.
void IntegratedMode::tickRace() {
  uint32_t now = millis();

  // Race start (from the web, or the countdown completing below): GO flash +
  // TRIPLE beep. Race stop (either path): TRIPLE beep. These transitions cover
  // both the OLED and web controllers since both drive _raceActive.
  if (_raceActive && !_prevRaceActive) {
    _goFlashUntilMs = now + RACE_GO_FLASH_MS;
    _lastBest3Ms = 0;  // new session: re-detect the first best-3
    if (_buzzer) {
      _buzzer->tripleBuzz();
    }
  } else if (!_raceActive && _prevRaceActive) {
    if (_buzzer) {
      _buzzer->tripleBuzz();
    }
  }
  _prevRaceActive = _raceActive;

  // Countdown progression: one beep per displayed second, then arm the race.
  if (_countdownActive) {
    uint32_t elapsed = now - _countdownStartMs;
    int remaining = RACE_COUNTDOWN_SECONDS - (int)(elapsed / 1000);  // 5,4,3,2,1
    int sec = (remaining > 0) ? remaining : 1;
    if (sec != _countdownLastBeepSecond) {
      _countdownLastBeepSecond = sec;
      if (_buzzer) {
        _buzzer->buzz();
      }
    }
    if (elapsed >= (uint32_t)RACE_COUNTDOWN_SECONDS * 1000) {
      // Countdown done: arm the race exactly as the web's start does.
      _countdownActive = false;
      _raceActive = true;
      _raceStartTime = now;
      _laps.clear();
      _lastBest3Ms = 0;
      _goFlashUntilMs = now + RACE_GO_FLASH_MS;  // show "Go!" immediately
    }
  }

  // Base status line.
  if (_countdownActive) {
    int remaining = RACE_COUNTDOWN_SECONDS - (int)((now - _countdownStartMs) / 1000);
    if (remaining < 1) {
      remaining = 1;
    }
    snprintf(_statusBuf, sizeof(_statusBuf), "Start in %d", remaining);
  } else if (_raceActive) {
    snprintf(_statusBuf, sizeof(_statusBuf),
             (now < _goFlashUntilMs) ? "Go!" : "Racing");
  } else {
    snprintf(_statusBuf, sizeof(_statusBuf), "Ready");
  }
}

#endif  // INTEGRATED
