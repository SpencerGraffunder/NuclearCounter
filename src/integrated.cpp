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

IntegratedMode::IntegratedMode(Settings *settings, RX5808 *rx)
  : _settings(settings), _rx(rx) {
  s_self = this;
}

void IntegratedMode::begin() {
  Serial.println(F("Hertz Hunter integrated firmware (scanner / USB node / WiFi timer)"));
  _mode = IntMode::SCANNER;
}

void IntegratedMode::process() {
  if (_mode == IntMode::NODE) {
    _node.process();
  } else if (_mode == IntMode::TIMER) {
    _timing.process();
  }
  // SCANNER: nothing — the scan task and menu own everything.
}

// Lazily start the timing engine and load the persisted band/channel/
// thresholds. Safe to call from both enterNode() and enterTimer(); the
// timing core keeps running across NODE<->TIMER switches so a race survives
// a mode change (and client disconnects — the task is independent of the
// web connection).
void IntegratedMode::ensureTiming() {
  if (!_timingBegun) {
    _timing.setRX5808(_rx);
    _timing.begin();
    _timing.setLapCallback(onTimingLap);
    _timingBegun = true;
  }
  _timingSettings.loadSettings(&_timing);
  _timing.setActivated(true);
}

void IntegratedMode::enterNode() {
  ensureTiming();
  _node.begin(&_timing);
  _mode = IntMode::NODE;
  Serial.println(F("Mode: USB node (RotorHazard)"));
}

void IntegratedMode::enterTimer() {
  ensureTiming();
  if (!_wifi.setupAP()) {
    Serial.println(F("WiFi AP failed to start - staying in scanner mode"));
    return;
  }
  _web.begin(&_timing, &_timingSettings, &_raceActive, &_raceStartTime, &_laps);
  _mode = IntMode::TIMER;
  Serial.printf("Mode: WiFi timer (%s)\n", WiFi.softAPIP().toString().c_str());
}

// Full radio shutdown, not just softAPdisconnect: on the S3, calling
// WiFi.mode(WIFI_AP) while a previous AP instance is still active crashes in
// ieee80211_hostap_attach.
void IntegratedMode::shutdownWifi() {
  WiFi.softAPdisconnect(true);
  delay(100);
  WiFi.mode(WIFI_OFF);
}

void IntegratedMode::exitToScanner() {
  if (_mode == IntMode::TIMER) {
    shutdownWifi();
  }
  _raceActive = false;
  _timing.setActivated(false);
  _mode = IntMode::SCANNER;
  Serial.println(F("Mode: scanner"));
}

void IntegratedMode::selectMode(IntMode m) {
  if (m == _mode) {
    exitToScanner();
    return;
  }
  if (_mode == IntMode::TIMER) {
    shutdownWifi();
  }
  _raceActive = false;
  if (m == IntMode::NODE) {
    enterNode();
  } else if (m == IntMode::TIMER) {
    enterTimer();
  }
}

bool IntegratedMode::pauseForCalibration() {
  if (_mode == IntMode::SCANNER || !_timingBegun) {
    return false;  // timing engine not running; nothing to pause
  }
  return _timing.pauseTemporarily(500);
}

void IntegratedMode::resumeAfterCalibration(bool wasActive) {
  if (_mode == IntMode::SCANNER || !_timingBegun) {
    return;
  }
  _timing.resumeFromPause(wasActive);
  // calibrate() tuned the hardware to 5800 MHz; force the timing core to
  // re-tune back to its tracked frequency (hardware and state can drift).
  _timing.setFrequency(_timing.getCurrentFrequency());
}

void IntegratedMode::addLap(const LapData &lap) {
  if (_mode != IntMode::TIMER || !_raceActive) {
    return;
  }
  _laps.push_back(lap);
  while (_laps.size() > 100) {
    _laps.erase(_laps.begin());
  }
}

#endif  // INTEGRATED
