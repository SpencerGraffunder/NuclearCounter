// Unit tests for ExtremumTracker (src/timing/timing_logic.h) — the RSSI
// peak/nadir history used by marshal mode. Native build: `pio test -e Test`.

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

void setUp(void) {}
void tearDown(void) {}

// Helper: drive one up/down wiggle so a peak (and usually a nadir) are
// finalized. Returns the peak just buffered.
static Extremum driveWiggle(ExtremumTracker& t, uint32_t t0, uint8_t base,
                            uint8_t top) {
  // rising: base -> top
  t.process(t0, base);
  t.process(t0 + 1, (uint8_t)((base + top) / 2));
  t.process(t0 + 2, top);
  // falling: top -> base
  t.process(t0 + 3, (uint8_t)((base + top) / 2));
  t.process(t0 + 4, base);
  return t.peekNextPeak();
}

void test_peak_buffered_on_direction_change(void) {
  // Fresh tracker starts at last_rssi=0. Exact trace:
  //   100:50 rising  -> peak tracking {50,100} (no buffer: current was invalid)
  //   110:80 rising  -> buffers {50,100,0}, peak tracking {80,110}
  //   120:70 falling -> finalizes peak {80,110,10} into the buffer
  ExtremumTracker t;
  t.process(100, 50);
  t.process(110, 80);
  t.process(120, 70);
  TEST_True(t.hasPendingPeak());
  Extremum p1 = t.peekNextPeak();
  TEST_Equal(p1.rssi, 50);
  TEST_Equal(p1.first_time, 100u);
  p1 = t.getNextPeak();
  Extremum p2 = t.peekNextPeak();
  TEST_Equal(p2.rssi, 80);
  TEST_Equal(p2.first_time, 110u);
  TEST_Equal(p2.duration, 10);
  TEST_True(p2.valid);
  t.getNextPeak();
  TEST_False(t.hasPendingPeak());
}

void test_nadir_buffered_on_direction_change(void) {
  //   120:70 falling -> nadir tracking {70,120}
  //   130:60 falling -> buffers {70,120,0}, nadir tracking {60,130}
  //   140:65 rising  -> finalizes nadir {60,130,10}
  ExtremumTracker t;
  t.process(100, 50);
  t.process(110, 80);
  t.process(120, 70);
  t.process(130, 60);
  t.process(140, 65);
  TEST_True(t.hasPendingNadir());
  t.getNextNadir();  // the {70,120,0} buffered on the 130 sample
  Extremum n = t.peekNextNadir();
  TEST_Equal(n.rssi, 60);
  TEST_Equal(n.first_time, 130u);
  TEST_Equal(n.duration, 10);
  TEST_True(n.valid);
}

void test_peek_does_not_consume_pop_does(void) {
  // driveWiggle(40 -> 120 -> 200 -> 120 -> 40) buffers THREE peaks: {40},
  // {120} (each buffered when the next higher sample arrives) and the
  // finalized {200}. Peek must not consume; pop must, in FIFO order.
  ExtremumTracker t;
  driveWiggle(t, 100, 40, 200);
  TEST_True(t.hasPendingPeak());
  Extremum first = t.peekNextPeak();
  TEST_Equal(first.rssi, 40);
  TEST_True(t.hasPendingPeak());  // peek keeps it
  TEST_Equal(t.peekNextPeak().rssi, 40);
  t.getNextPeak();  // {40}
  t.getNextPeak();  // {120}
  Extremum last = t.peekNextPeak();
  TEST_Equal(last.rssi, 200);
  Extremum popped = t.getNextPeak();
  TEST_Equal(popped.rssi, 200);
  TEST_False(t.hasPendingPeak());
  Extremum empty = t.getNextPeak();
  TEST_False(empty.valid);
  TEST_Equal(empty.rssi, 0);
}

void test_empty_pop_sentinels(void) {
  ExtremumTracker t;
  Extremum p = t.getNextPeak();
  TEST_Equal(p.rssi, 0);
  TEST_False(p.valid);
  Extremum n = t.getNextNadir();
  TEST_Equal(n.rssi, 255);
  TEST_False(n.valid);
}

void test_flat_extends_duration(void) {
  ExtremumTracker t;
  t.process(100, 50);
  t.process(110, 200);  // buffers {50,100,0}; peak starts at 200
  t.process(120, 200);  // flat: duration 10
  t.process(130, 200);  // flat: duration 20
  t.process(131, 190);  // falling: finalizes peak with duration 21
  t.getNextPeak();  // consume the {50,100,0} buffered on the 110 sample
  Extremum p = t.peekNextPeak();
  TEST_Equal(p.rssi, 200);
  TEST_Equal(p.first_time, 110u);
  TEST_Equal(p.duration, 21);
}

void test_flat_max_duration_rebuffers(void) {
  // A flat extremum held > 0xFFFF ms must buffer a 0xFFFF chunk and start a
  // new one (duration is uint16).
  ExtremumTracker t;
  t.process(0, 50);
  t.process(1, 200);  // buffers {50}; peak tracking starts at 200
  t.getNextPeak();    // consume the {50} so peeks see the flat chunks
  bool saw_max_chunk = false;
  for (uint32_t ts = 2; ts < 70000; ts++) {
    t.process(ts, 200);
    if (t.hasPendingPeak() && t.peekNextPeak().duration == 0xFFFF) {
      saw_max_chunk = true;
      break;
    }
  }
  TEST_True(saw_max_chunk);
}

void test_no_phantom_extremums_on_first_sample(void) {
  // The current-peak/nadir trackers start invalid, so the very first rising
  // (or falling) sample must not buffer a phantom extremum from the
  // uninitialized state.
  ExtremumTracker t;
  t.process(0, 100);  // first sample: rising from last_rssi=0
  TEST_False(t.hasPendingPeak());
  TEST_False(t.hasPendingNadir());

  // Same from a fresh reset.
  t.reset();
  t.process(0, 0);  // flat at 0
  t.process(1, 5);  // rising
  TEST_False(t.hasPendingPeak());
  TEST_False(t.hasPendingNadir());
}

void test_ring_overflow_drops_oldest(void) {
  ExtremumTracker t;
  // Generate EXTREMUM_BUFFER_SIZE+10 distinct peaks without consuming; the
  // oldest must drop off.
  for (int i = 0; i < EXTREMUM_BUFFER_SIZE + 10; i++) {
    uint32_t t0 = (uint32_t)(i * 20);
    uint8_t top = (uint8_t)(150 + (i % 20));  // distinct peak values
    driveWiggle(t, t0, 40, top);
  }
  // The first (lowest) peaks are gone; the newest is on top.
  bool saw_newest = false;
  for (int i = 0; i < EXTREMUM_BUFFER_SIZE + 10; i++) {
    if (!t.hasPendingPeak()) break;
    Extremum p = t.getNextPeak();
    if (!p.valid) continue;
    if (p.rssi == 150 + (uint8_t)((EXTREMUM_BUFFER_SIZE + 9) % 20)) {
      saw_newest = true;
    }
  }
  TEST_True(saw_newest);
}

void test_reset_clears_everything(void) {
  ExtremumTracker t;
  driveWiggle(t, 100, 40, 200);
  TEST_True(t.hasPendingPeak());
  t.reset();
  TEST_False(t.hasPendingPeak());
  TEST_False(t.hasPendingNadir());
  TEST_Equal(t.lastRssi(), 0);
  TEST_Equal(t.rssiChange(), 0);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_peak_buffered_on_direction_change);
  RUN_TEST(test_nadir_buffered_on_direction_change);
  RUN_TEST(test_peek_does_not_consume_pop_does);
  RUN_TEST(test_empty_pop_sentinels);
  RUN_TEST(test_flat_extends_duration);
  RUN_TEST(test_flat_max_duration_rebuffers);
  RUN_TEST(test_no_phantom_extremums_on_first_sample);
  RUN_TEST(test_ring_overflow_drops_oldest);
  RUN_TEST(test_reset_clears_everything);
  return UNITY_END();
}
