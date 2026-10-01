#ifndef INTEGRATED_H
#define INTEGRATED_H

#ifdef INTEGRATED

#include <Arduino.h>
#include <vector>
#include "settings.h"
#include "RX5808.h"
#include "timing_core.h"
#include "node_mode.h"
#include "settings/settings_manager.h"
#include "settings/wifi_manager.h"
#include "web/web_server.h"

// Background operating modes of the integrated firmware.
// Exactly one "owner" drives the RX5808 at any time:
//   SCANNER -> the NC scan task (frequency sweep, bar-graph page)
//   NODE    -> TimingCore (RotorHazard USB node, fixed VTX channel)
//   TIMER   -> TimingCore (WiFi web timer, fixed VTX channel)
enum class IntMode {
  SCANNER,
  NODE,
  TIMER,
};

// Orchestration layer between the NC shell (menu, buttons, battery, buzzer)
// and the SFOS-ported modules (TimingCore, NodeMode, web stack).
class IntegratedMode {
public:
  IntegratedMode(Settings *settings, RX5808 *rx);

  // Start the firmware in scanner mode (call once from setup()).
  void begin();

  // Per-loop work: node serial protocol / timing bookkeeping.
  void process();

  IntMode mode() const { return _mode; }

  // SELECT on a MAIN-menu mode row: enter the mode if it is not the active
  // one, exit to scanner mode if it already is (toggle semantics).
  void selectMode(IntMode m);

  // Calibration guard: while NODE/TIMER is active the 1ms timing task bit-bangs
  // the same RX5808 pins that RX5808::calibrate() uses, so pause it around the
  // call and re-tune afterwards (calibrate() leaves the hardware at 5800 MHz).
  bool pauseForCalibration();
  void resumeAfterCalibration(bool wasActive);

  // Status-page data (shown on the SCAN page while NODE/TIMER is active)
  uint16_t frequencyMhz() const { return _timing.getCurrentFrequency(); }
  uint8_t rssi() const { return _timing.getCurrentRSSI(); }
  uint16_t lapCount() const { return _timing.getLapCount(); }
  uint32_t lastLapTimeMs() { return _timing.getLastLap().lap_time_ms; }
  String apIP() const { return WiFi.softAPIP().toString(); }

  // Called from the static timing lap callback (onTimingLap). Public because
  // that callback is a plain C function pointer, not a member/friend.
  void addLap(const LapData &lap);

private:
  void ensureTiming();
  void enterNode();
  void enterTimer();
  void exitToScanner();
  void shutdownWifi();

  Settings *_settings;  // NC settings (scan interval etc., scanner mode)
  RX5808 *_rx;          // NC RX5808 (scan task, calibration, RF tuning)

  TimingCore _timing;
  NodeMode _node;
  SettingsManager _timingSettings;  // NVS "sfos": band/channel/thresholds
  WiFiManager _wifi;
  WebServerManager _web;

  IntMode _mode = IntMode::SCANNER;
  bool _timingBegun = false;

  // Race state shared with the web server (web start/stop-race semantics).
  // In NODE mode laps are consumed directly by the node protocol; in TIMER
  // mode the timing lap callback feeds this vector for the web UI.
  std::vector<LapData> _laps;
  bool _raceActive = false;
  uint32_t _raceStartTime = 0;
};

#endif  // INTEGRATED
#endif  // INTEGRATED_H
