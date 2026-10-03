#ifdef INTEGRATED

#include "integrated.h"
#include "about.h"
#include <WiFi.h>

// Firmware info strings reported over the RotorHazard protocol
// (READ_FIRMWARE_INFO) — externs declared in node_mode.cpp.
const char *firmwareVersionString = VERSION;
const char *firmwareBuildDateString = __DATE__;
const char *firmwareBuildTimeString = __TIME__;
const char *firmwareProcTypeString = "ESP32";

// The TimingCore lap callback is a plain C function pointer, so it needs a
// global handle to reach the single IntegratedMode instance.
static IntegratedMode *s_self = nullptr;

// TIMER mode only: feed the web UI's lap vector. In NODE mode the node
// protocol consumes laps directly from the timing core's lap queue.
static void onTimingLap(const LapData &lap) {
  if (s_self) {
    s_self->addLap(lap);
  }
}

IntegratedMode::IntegratedMode(Settings *settings, RX5808 *rx, Buzzer *buzzer)
  : _settings(settings), _rx(rx), _buzzer(buzzer) {
  s_self = this;
}

void IntegratedMode::begin() {
  Serial.println(F("Hertz Hunter integrated firmware (USB node always on, scanner + WiFi timer pages)"));

  // Pre-initialize the WiFi AP + web server BEFORE the timing task starts:
  //  - SFOS ordering: the high-priority timing task must not be running while
  //    the WiFi driver initialises (it starves the init)
  //  - the AP is then never reconfigured for the whole boot session, which
  //    sidesteps the S3 ieee80211_hostap_attach crash on AP restart
  //  - the WiFi Timer page is instant to open afterwards, and the network is
  //    always available (web timer keeps running in any mode; a client can
  //    reconnect and catch up)
  if (_wifi.setupAP()) {
    _web.begin(&_timing, &_timingSettings, &_raceActive, &_raceStartTime, &_laps);
    _webBegun = true;
  } else {
    Serial.println(F("WiFi AP failed to start at boot - will retry when the timer page is first opened"));
  }

  startNode();
}

void IntegratedMode::process() {
  tickRace();  // countdown / go-flash / lap-flash / status line (runs in all modes)
  if (_mode == IntMode::NODE) {
    _node.process();  // includes handleSerialInput()
  } else {
    // SCANNING / TIMER: the RotorHazard host is not in control; discard any
    // protocol bytes so stale commands (e.g. WRITE_FREQUENCY) can't steer the
    // scanner or timer. The USB CDC port itself stays alive (esptool
    // auto-download-mode flashing depends on it).
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
  Serial.println(F("Node baseline active"));
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

// WiFi timer page entry. Normally instant (AP + web were pre-initialized at
// boot); only the boot-failure fallback does the slow full bring-up while
// the menu splash is visible.
bool IntegratedMode::enterTimer() {
  if (_mode == IntMode::TIMER) {
    return true;  // already up (re-press guard)
  }
  if (_mode == IntMode::SCANNING) {
    _rx->stopScan();
    delay(5);  // let the scan task finish any in-flight bit-bang
  }
  if (!_webBegun) {
    // AP didn't come up at boot — full (slow) bring-up now.
    Serial.println(F("Starting WiFi timer..."));
    if (!_wifi.setupAP()) {
      Serial.println(F("WiFi AP failed to start"));
      startNode();
      return false;
    }
    _web.begin(&_timing, &_timingSettings, &_raceActive, &_raceStartTime, &_laps);
    _webBegun = true;
  }
  _timing.setActivated(true);
  // Defensive re-tune: if the scanner had driven the RF pins, the hardware
  // is left on the sweep's last frequency.
  _timing.setFrequency(_timing.getCurrentFrequency());
  _mode = IntMode::TIMER;
  Serial.printf("Mode: WiFi timer (%s)\n", WiFi.softAPIP().toString().c_str());
  return true;
}

// WiFi timer page exit: the WiFi stack STAYS UP (tearing the AP down to
// re-create it is the path that crashes the S3, and the web timer is meant
// to keep running in the background — a client can reconnect and catch up).
// Only the node serial protocol resumes.
void IntegratedMode::exitTimer() {
  if (_mode != IntMode::TIMER) {
    return;
  }
  startNode();
  Serial.println(F("Node resumed (WiFi timer stays up in the background)"));
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
  // Lap-crossing feedback: beep + a transient status flash. Works in any menu
  // mode (the pilot hears it), not just while the timer page is up.
  if (_buzzer) {
    _buzzer->buzz();
  }
  snprintf(_statusFlash, sizeof(_statusFlash), "Lap %d", (int)_laps.size());
  _statusFlashUntilMs = millis() + RACE_LAP_FLASH_MS;
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

// Advance the race session state machine and refresh the status line. Called
// from process() every loop, so it runs regardless of which menu page is up.
void IntegratedMode::tickRace() {
  uint32_t now = millis();

  // A race just started (from the web, or the countdown completing below):
  // flash "Go!" and beep once. This is the single Go-beep source for both.
  if (_raceActive && !_prevRaceActive) {
    _goFlashUntilMs = now + RACE_GO_FLASH_MS;
    if (_buzzer) {
      _buzzer->buzz();
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

  // A transient lap flash (set in addLap) overrides the base status.
  if (now < _statusFlashUntilMs) {
    strncpy(_statusBuf, _statusFlash, sizeof(_statusBuf) - 1);
    _statusBuf[sizeof(_statusBuf) - 1] = '\0';
  }
}

#endif  // INTEGRATED
