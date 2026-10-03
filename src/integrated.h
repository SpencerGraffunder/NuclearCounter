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

// Effective state of the integrated firmware. The RotorHazard USB node is the
// always-on baseline: it runs continuously from boot. The two menu pages that
// need exclusive ownership of the RF hardware temporarily override it:
//   SCANNING -> the NC scan task sweeps the RX5808; node timing is paused and
//               the serial protocol is drained (a WRITE_FREQUENCY arriving
//               mid-sweep would steer the scanner).
//   TIMER    -> the WiFi web timer owns the timing engine; the node protocol
//               is off and serial is drained.
enum class IntMode {
  NODE,      // baseline, always-on RotorHazard USB node
  SCANNING,  // temporary: scanner page is up
  TIMER,     // temporary: WiFi timer page is up
};

// Orchestration layer between the NC shell (menu, buttons, battery, buzzer)
// and the SFOS-ported modules (TimingCore, NodeMode, web stack).
class IntegratedMode {
public:
  IntegratedMode(Settings *settings, RX5808 *rx);

  // Start the firmware with the node baseline running (call once from setup()).
  void begin();

  // Per-loop work: node protocol / timing bookkeeping / serial draining.
  void process();

  IntMode mode() const { return _mode; }

  // Scanner page transitions. The menu owns the scan task itself
  // (module->startScan()/stopScan()); these manage the node pause/resume.
  void enterScan();
  void exitScan();

  // WiFi timer page transitions. enterTimer() is instant when the AP was
  // pre-initialized at boot; only the boot-failure fallback blocks for the
  // full (slow) bring-up. Returns false if the AP failed to start.
  bool enterTimer();
  void exitTimer();

  // Calibration guard: while the timing engine is active its 1ms task
  // bit-bangs the same RX5808 pins that RX5808::calibrate() uses, so pause it
  // around the call and re-tune afterwards (calibrate() leaves the hardware
  // at 5800 MHz).
  bool pauseForCalibration();
  void resumeAfterCalibration(bool wasActive);

  // Status-page data
  uint16_t frequencyMhz() const { return _timing.getCurrentFrequency(); }
  uint8_t rssi() const { return _timing.getCurrentRSSI(); }
  uint16_t lapCount() const { return _timing.getLapCount(); }
  uint32_t lastLapTimeMs() { return _timing.getLastLap().lap_time_ms; }
  String apIP() const { return WiFi.softAPIP().toString(); }

  // Called from the static timing lap callback (onTimingLap). Public because
  // that callback is a plain C function pointer, not a member/friend.
  void addLap(const LapData &lap);

private:
  // (Re)start the always-on node baseline. Idempotent.
  void startNode();
  void drainSerial();

  Settings *_settings;  // NC settings (scan interval etc., scanner page)
  RX5808 *_rx;          // NC RX5808 (scan task, calibration, RF tuning)

  TimingCore _timing;
  NodeMode _node;
  SettingsManager _timingSettings;  // NVS "sfos": band/channel/thresholds
  WiFiManager _wifi;
  WebServerManager _web;

  IntMode _mode = IntMode::NODE;
  bool _timingBegun = false;
  bool _webBegun = false;

  // Race state shared with the web server (web start/stop-race semantics).
  // In NODE mode laps are consumed directly by the node protocol; in TIMER
  // mode the timing lap callback feeds this vector for the web UI.
  std::vector<LapData> _laps;
  bool _raceActive = false;
  uint32_t _raceStartTime = 0;
};

#endif  // INTEGRATED
#endif  // INTEGRATED_H
