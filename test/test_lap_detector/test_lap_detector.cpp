// Unit tests for LapDetector (src/timing/timing_logic.h) — the per-sample
// lap-detection state machine. These run natively (`pio test -e Test`),
// i.e. on the host CPU with no ESP hardware, exercising the SAME code the
// firmware links.
//
// Thresholds in the tests: enter=120, exit=100, min_lap=0, hard min=3000 ms
// (the firmware defaults). Sample times are in "ms" and advance 1 ms at a
// time, mirroring the firmware's 1 ms cadence.

// Unity 2.6 dropped the legacy TEST_* aliases; map the familiar short
// names onto the current TEST_ASSERT_* ones.
#define TEST_TRUE(cond)  TEST_ASSERT_TRUE(cond)
#define TEST_True(cond)  TEST_ASSERT_TRUE(cond)
#define TEST_FALSE(cond) TEST_ASSERT_FALSE(cond)
#define TEST_Equal(a, b) TEST_ASSERT_EQUAL(a, b)
#define TEST_False(cond) TEST_ASSERT_FALSE(cond)
#define TEST_EqualFloat(a, b, tol) TEST_ASSERT_FLOAT_WITHIN(tol, (a), (b))

#include <unity.h>

#include "../../src/timing/timing_logic.h"

// Globals for the C-style callbacks.
static int g_cross_started = 0;
static int g_cross_ended = 0;
static uint8_t g_cross_start_rssi = 0;
static uint8_t g_cross_end_rssi = 0;
static int g_cb_calls = 0;
static uint8_t g_cb_peak = 0;
static bool g_cb_valid = false;

namespace {

LapDetector::Events feed(LapDetector& d, uint32_t t, uint8_t rssi) {
  return d.process(t, rssi);
}

// Drive one clean crossing:
//   t+0 baseline (50) | t+1 step to `peak` (crossing starts, peak captured)
//   t+2 plateau       | t+3 drop to `low` (< exit -> capture, record if ok)
// Peak timestamp is therefore t+1. Returns the drop sample's event.
LapDetector::Events driveCrossing(LapDetector& d, uint32_t t, uint8_t peak,
                                  uint8_t low, int* laps = nullptr) {
  LapDetector::Events ev;
  ev = feed(d, t, 50);
  ev = feed(d, t + 1, peak);
  ev = feed(d, t + 2, peak);
  ev = feed(d, t + 3, low);
  if (ev.lap_recorded && laps) (*laps)++;
  return ev;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_defaults_match_firmware_config(void) {
  LapDetector d;
  TEST_Equal(d.enterRssi(), 120);   // ENTER_RSSI
  TEST_Equal(d.exitRssi(), 100);    // EXIT_RSSI
  TEST_Equal(d.minLapMs(), 0);      // MIN_LAP_MS
  TEST_Equal(d.nadirRssi(), 255);
  TEST_Equal(d.passNadirRssi(), 255);
  TEST_Equal(d.lapCount(), 0);
  TEST_Equal(d.rssiPeak(), 0);
  TEST_False(d.crossingActive());
}

void test_noise_below_enter_never_laps(void) {
  LapDetector d;
  d.setRaceStartMs(1000);
  // Deterministic pseudo-noise well below the enter threshold.
  for (uint32_t t = 0; t < 5000; t++) {
    LapDetector::Events ev = feed(d, t, (uint8_t)((t * 7) % 90));
    TEST_False(ev.lap_recorded);
    TEST_False(ev.crossing_started);
  }
  TEST_Equal(d.lapCount(), 0);
}

void test_single_lap_peak_timestamp_and_time(void) {
  LapDetector d;
  d.setRaceStartMs(1000);
  int laps = 0;
  LapDetector::Events ev = driveCrossing(d, 2000, 150, 90, &laps);

  TEST_True(ev.lap_recorded);
  TEST_Equal(laps, 1);
  TEST_Equal(d.lapCount(), 1);

  const LapData& lap = d.lastLap();
  TEST_True(lap.valid);
  // Lap time is the PEAK time (t+1 = 2001), not the drop time (2003).
  TEST_Equal(lap.timestamp_ms, 2001u);
  // First lap time is measured from the race start.
  TEST_Equal(lap.lap_time_ms, 1001u);
  TEST_Equal(lap.rssi_peak, 150);
  TEST_Equal(lap.pilot_id, 0);
}

void test_peak_across_sampling_gap(void) {
  // The model may be fully past the gate in a single 1 ms step: RSSI goes
  // straight from baseline to above-enter and back to below-exit with no
  // intermediate samples. The peak must still be captured at the one sample
  // that saw it.
  LapDetector d;
  d.setRaceStartMs(0);
  feed(d, 0, 40);
  LapDetector::Events ev = feed(d, 1, 180);  // spike (above enter)
  TEST_False(ev.lap_recorded);  // still above exit, nothing captured yet
  ev = feed(d, 2, 60);          // gone (below exit)
  TEST_True(ev.lap_recorded);
  TEST_Equal(d.lapCount(), 1);
  TEST_Equal(d.lastLap().timestamp_ms, 1u);  // the spike sample
  TEST_Equal(d.lastLap().rssi_peak, 180);
}

void test_plateau_peak_uses_first_max_sample(void) {
  // Peak capture uses a STRICT `>`, so a plateau records the FIRST sample at
  // the maximum, not the last.
  LapDetector d;
  d.setRaceStartMs(0);
  feed(d, 0, 50);
  feed(d, 1, 130);
  feed(d, 2, 150);
  feed(d, 3, 150);  // same value: must NOT update the peak time
  feed(d, 4, 150);
  LapDetector::Events ev = feed(d, 5, 90);
  TEST_True(ev.lap_recorded);
  TEST_Equal(d.lastLap().timestamp_ms, 2u);
  TEST_Equal(d.lastLap().rssi_peak, 150);
}

void test_no_lap_unless_rssi_drops_below_exit(void) {
  LapDetector d;
  d.setRaceStartMs(0);
  feed(d, 0, 50);
  feed(d, 1, 130);
  feed(d, 2, 150);
  // Hold above the exit threshold forever: peak never "captured".
  for (uint32_t t = 3; t < 200; t++) {
    LapDetector::Events ev = feed(d, t, 110);
    TEST_False(ev.lap_recorded);
  }
  TEST_Equal(d.lapCount(), 0);
  // A real drop later still records the lap with the original peak time.
  LapDetector::Events ev = feed(d, 200, 90);
  TEST_True(ev.lap_recorded);
  TEST_Equal(d.lastLap().timestamp_ms, 2u);
}

void test_crossing_events_start_and_end(void) {
  LapDetector d;
  d.setCrossingCallback([](bool active, uint8_t rssi) {
    if (active) {
      g_cross_started++;
      g_cross_start_rssi = rssi;
    } else {
      g_cross_ended++;
      g_cross_end_rssi = rssi;
    }
  });

  feed(d, 0, 50);
  LapDetector::Events ev = feed(d, 1, 130);  // >= enter
  TEST_True(ev.crossing_started);
  TEST_False(ev.crossing_ended);
  TEST_True(d.crossingActive());
  TEST_Equal(d.crossingStart(), 1u);

  ev = feed(d, 2, 150);
  TEST_False(ev.crossing_started);
  TEST_False(ev.crossing_ended);

  ev = feed(d, 3, 90);  // < enter
  TEST_False(ev.crossing_started);
  TEST_True(ev.crossing_ended);
  TEST_False(d.crossingActive());

  TEST_Equal(g_cross_started, 1);
  TEST_Equal(g_cross_ended, 1);
  TEST_Equal(g_cross_start_rssi, 130);
  TEST_Equal(g_cross_end_rssi, 90);
}

void test_min_lap_gate_blocks_capture(void) {
  // Isolate the configurable min-lap capture gate by dropping the hard
  // record minimum to 0 (the 3000 ms anti-bounce is covered separately).
  LapDetector d;
  d.setRaceStartMs(0);
  d.setMinLapMs(2000);
  d.setHardMinLapMs(0);

  // First lap (no gate yet: lap_start is 0). Peak at t+1 = 101.
  int laps = 0;
  driveCrossing(d, 100, 150, 90, &laps);
  TEST_Equal(laps, 1);
  TEST_Equal(d.lapStartTimeMs(), 101u);

  // Crossing 199 ms after the lap start: inside the gate. No peak may be
  // captured and no crossing may even start (crossing_active requires
  // can_capture_peak).
  LapDetector::Events ev = feed(d, 300, 130);
  TEST_False(ev.crossing_started);
  TEST_False(d.crossingActive());
  TEST_Equal(d.rssiPeak(), 0);  // nothing captured
  feed(d, 301, 150);
  TEST_Equal(d.rssiPeak(), 0);
  feed(d, 302, 90);
  TEST_Equal(d.lapCount(), 1);

  // Boundary: exactly min_lap_ms after lap start the gate opens.
  // 101 + 2000 = 2101 -> allowed; 2100 -> still gated.
  ev = feed(d, 2100, 130);
  TEST_False(ev.crossing_started);
  ev = feed(d, 2101, 130);
  TEST_True(ev.crossing_started);

  // Finish this crossing; the lap must be recorded with its own peak.
  feed(d, 2102, 150);
  ev = feed(d, 2103, 90);
  TEST_True(ev.lap_recorded);
  TEST_Equal(d.lapCount(), 2);
  TEST_Equal(d.lastLap().timestamp_ms, 2102u);
}

void test_hard_min_rejects_then_records_late(void) {
  // Pinned quirk: a lap rejected as "too soon" does NOT clear the captured
  // peak. A later sample below that stale peak records the lap LATE, with
  // the original peak timestamp. (See the header comment in timing_logic.h.)
  LapDetector d;
  d.setRaceStartMs(0);
  int laps = 0;

  // Lap 1: peak at 101, recorded at 103.
  driveCrossing(d, 100, 150, 90, &laps);
  TEST_Equal(laps, 1);

  // Lap 2 crossing ~300 ms later: peak captured (min_lap gate is off by
  // default) but the record is rejected (< 3000 ms since lap 1's peak).
  feed(d, 400, 130);
  feed(d, 401, 135);  // peak 135 @ 401
  LapDetector::Events ev = feed(d, 402, 90);
  TEST_True(ev.lap_rejected);
  TEST_False(ev.lap_recorded);
  TEST_Equal(ev.since_last_lap_ms, 301u);  // 402 - 101
  TEST_Equal(d.lapCount(), 1);
  TEST_Equal(d.rssiPeak(), 135);  // stale peak retained

  // RSSI stays below exit. Once 3000 ms have passed since lap 1's peak
  // (101 + 3000 = 3101), the very next below-exit sample records the late
  // lap using the OLD peak time (401).
  for (uint32_t t = 403; t < 3101; t++) {
    ev = feed(d, t, 90);
    TEST_False(ev.lap_recorded);
  }
  ev = feed(d, 3101, 90);
  TEST_True(ev.lap_recorded);
  TEST_Equal(d.lapCount(), 2);
  TEST_Equal(d.lastLap().timestamp_ms, 401u);  // original peak time
  TEST_Equal(d.lastLap().lap_time_ms, 300u);   // 401 - 101 (lap 1 peak)
  TEST_Equal(d.lastLap().rssi_peak, 135);
}

void test_second_lap_time_from_previous_peak(void) {
  LapDetector d;
  d.setRaceStartMs(5000);
  int laps = 0;
  driveCrossing(d, 10000, 150, 90, &laps);  // peak 10001
  TEST_Equal(d.lastLap().lap_time_ms, 5001u);  // from race start
  driveCrossing(d, 14000, 140, 90, &laps);     // peak 14001
  TEST_Equal(laps, 2);
  TEST_Equal(d.lastLap().lap_time_ms, 4000u);  // 14001 - 10001
}

void test_first_lap_zero_time_without_race_start(void) {
  LapDetector d;
  // raceStartMs left 0 -> first lap time is 0 by contract.
  int laps = 0;
  driveCrossing(d, 1000, 150, 90, &laps);
  TEST_Equal(laps, 1);
  TEST_Equal(d.lastLap().lap_time_ms, 0u);
}

void test_millis_wraparound(void) {
  // The firmware runs for days; uint32 ms arithmetic must wrap correctly.
  LapDetector d;
  d.setRaceStartMs(0xFFFFF000u);
  int laps = 0;
  LapDetector::Events ev = feed(d, 0xFFFFF100u, 50);
  (void)ev;
  feed(d, 0xFFFFF101u, 130);
  feed(d, 0xFFFFF102u, 150);  // peak
  feed(d, 0xFFFFF103u, 150);
  ev = feed(d, 0x00000000u, 90);  // time wraps
  TEST_True(ev.lap_recorded);
  TEST_Equal(laps, 0);
  // peak (0xFFFFF102) - race start (0xFFFFF000) = 0x102 = 258 ms
  TEST_Equal(d.lastLap().lap_time_ms, 258u);
  // Since-last-lap on the first lap is UINT32_MAX by contract.
  TEST_Equal(ev.since_last_lap_ms, UINT32_MAX);
}

void test_nadir_tracking_and_reset_after_lap(void) {
  LapDetector d;
  feed(d, 0, 30);
  TEST_Equal(d.nadirRssi(), 30);
  TEST_Equal(d.passNadirRssi(), 30);
  feed(d, 1, 50);
  TEST_Equal(d.nadirRssi(), 30);  // global nadir never rises

  int laps = 0;
  driveCrossing(d, 100, 150, 90, &laps);
  TEST_Equal(laps, 1);
  TEST_Equal(d.passNadirRssi(), 255);  // reset by the lap
  TEST_Equal(d.nadirRssi(), 30);       // global nadir survives

  // A later lower sample updates both.
  feed(d, 5000, 10);
  TEST_Equal(d.nadirRssi(), 10);
  TEST_Equal(d.passNadirRssi(), 10);
}

void test_reset_keeps_configuration(void) {
  LapDetector d;
  d.setEnterRssi(110);
  d.setExitRssi(90);
  d.setMinLapMs(1500);
  d.setHardMinLapMs(0);
  d.setRaceStartMs(1234);
  int laps = 0;
  driveCrossing(d, 1000, 150, 80, &laps);
  TEST_Equal(laps, 1);

  d.reset();
  TEST_Equal(d.lapCount(), 0);
  TEST_Equal(d.lastLapTime(), 0);
  TEST_Equal(d.peakRssi(), 0);
  TEST_Equal(d.nadirRssi(), 255);
  TEST_Equal(d.passNadirRssi(), 255);
  TEST_False(d.crossingActive());
  TEST_Equal(d.rssiPeak(), 0);
  TEST_Equal(d.lapStartTimeMs(), 0);
  TEST_Equal(d.raceStartMs(), 0);
  // Configuration deliberately survives reset().
  TEST_Equal(d.enterRssi(), 110);
  TEST_Equal(d.exitRssi(), 90);
  TEST_Equal(d.minLapMs(), 1500);
}

void test_lap_callback_receives_recorded_lap(void) {
  LapDetector d;
  d.setRaceStartMs(0);
  d.setLapCallback([](const LapData& lap) {
    g_cb_calls++;
    g_cb_peak = lap.rssi_peak;
    g_cb_valid = lap.valid;
  });
  int laps = 0;
  driveCrossing(d, 100, 150, 90, &laps);
  TEST_Equal(g_cb_calls, 1);  // fired inside process()
  TEST_Equal(g_cb_peak, 150);
  TEST_True(g_cb_valid);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_defaults_match_firmware_config);
  RUN_TEST(test_noise_below_enter_never_laps);
  RUN_TEST(test_single_lap_peak_timestamp_and_time);
  RUN_TEST(test_peak_across_sampling_gap);
  RUN_TEST(test_plateau_peak_uses_first_max_sample);
  RUN_TEST(test_no_lap_unless_rssi_drops_below_exit);
  RUN_TEST(test_crossing_events_start_and_end);
  RUN_TEST(test_min_lap_gate_blocks_capture);
  RUN_TEST(test_hard_min_rejects_then_records_late);
  RUN_TEST(test_second_lap_time_from_previous_peak);
  RUN_TEST(test_first_lap_zero_time_without_race_start);
  RUN_TEST(test_millis_wraparound);
  RUN_TEST(test_nadir_tracking_and_reset_after_lap);
  RUN_TEST(test_reset_keeps_configuration);
  RUN_TEST(test_lap_callback_receives_recorded_lap);
  return UNITY_END();
}
