// Unit tests for LapRingBuffer (src/timing/timing_logic.h) — the lap FIFO
// that used to be lap_buffer[]/write_index/read_index in TimingCore.
//
// This ring's overflow semantics are load-bearing: when more than
// MAX_LAPS_STORED laps are recorded before being consumed, the writer
// overwrites the oldest unconsumed lap. (The >50-lap RotorHazard freeze was
// fixed by forwarding laps in real time, but the ring itself keeps these
// semantics.)

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

namespace {
LapData lap(int n) {
  return LapData{(uint32_t)n * 1000, (uint16_t)(n * 10), (uint8_t)(100 + n),
                 0, true};
}
}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_fifo_order(void) {
  LapRingBuffer<50> r;
  TEST_False(r.hasNew());
  TEST_Equal(r.available(), 0);
  LapData empty = r.pop();  // pop on empty is safe, invalid
  TEST_False(empty.valid);

  r.push(lap(1));
  r.push(lap(2));
  r.push(lap(3));
  TEST_Equal(r.available(), 3);
  TEST_Equal(r.pop().timestamp_ms, 1000u);
  TEST_Equal(r.pop().timestamp_ms, 2000u);
  TEST_Equal(r.pop().timestamp_ms, 3000u);
  TEST_False(r.hasNew());
}

void test_last_returns_most_recent(void) {
  LapRingBuffer<50> r;
  r.push(lap(1));
  r.push(lap(2));
  TEST_Equal(r.last().timestamp_ms, 2000u);
  // Reading does not disturb last().
  TEST_Equal(r.pop().timestamp_ms, 1000u);
  TEST_Equal(r.last().timestamp_ms, 2000u);
}

void test_overflow_drops_oldest_unconsumed(void) {
  // 60 laps, none consumed. The write index wraps past the read index, so
  // the only readable entries are those between read (0) and write (10):
  // laps 51..60. Everything older was silently overwritten.
  LapRingBuffer<50> r;
  for (int i = 1; i <= 60; i++) r.push(lap(i));
  TEST_Equal(r.available(), 10);

  int got = 0;
  int first = 0;
  while (r.hasNew()) {
    LapData l = r.pop();
    if (got == 0) first = (int)(l.timestamp_ms / 1000);
    got++;
  }
  TEST_Equal(got, 10);
  TEST_Equal(first, 51);  // laps 1..50 were overwritten
}

void test_full_ring_reports_empty(void) {
  // THE historical freeze, pinned: when exactly capacity laps are recorded
  // without being consumed, the write index lands back on the read index and
  // hasNew() reports FALSE — the ring looks empty even though 50 laps were
  // lost. This is why the firmware forwards laps to the node in real time
  // (onTimingLap) instead of draining this ring in batches: a batch drain
  // during a >50-lap session silently froze the RotorHazard lap counter.
  LapRingBuffer<50> r;
  for (int i = 1; i <= 50; i++) r.push(lap(i));
  TEST_False(r.hasNew());
  TEST_Equal(r.available(), 0);
}

void test_sustained_wrap_many_rounds(void) {
  // The historical freeze: >50 total laps used to make hasNewLap() go
  // permanently false. Push/read in lockstep for 500 laps and confirm the
  // stream never stalls and order is preserved.
  LapRingBuffer<50> r;
  for (int i = 1; i <= 500; i++) {
    r.push(lap(i));
    TEST_True(r.hasNew());
    LapData l = r.pop();
    TEST_Equal(l.timestamp_ms, (uint32_t)i * 1000u);
  }
  TEST_False(r.hasNew());
}

void test_reset_clears(void) {
  LapRingBuffer<50> r;
  for (int i = 1; i <= 10; i++) r.push(lap(i));
  r.reset();
  TEST_False(r.hasNew());
  TEST_Equal(r.available(), 0);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_fifo_order);
  RUN_TEST(test_last_returns_most_recent);
  RUN_TEST(test_overflow_drops_oldest_unconsumed);
  RUN_TEST(test_full_ring_reports_empty);
  RUN_TEST(test_sustained_wrap_many_rounds);
  RUN_TEST(test_reset_clears);
  return UNITY_END();
}
