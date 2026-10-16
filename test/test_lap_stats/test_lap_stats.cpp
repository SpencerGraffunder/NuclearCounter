// Unit tests for LapStats (src/timing/timing_logic.h) — best lap and
// best-3-consecutive used by the OLED timer page and the web API. Includes
// a seeded property test against a brute-force reference implementation.

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

#include <cstdlib>
#include <vector>

namespace {

LapData lap(uint32_t ms, bool valid = true) {
  return LapData{0, (uint16_t)(ms > 0xFFFF ? 0xFFFF : ms), 0, 0, valid};
}

// Brute-force reference: min over all i where i, i+1, i+2 are all valid and
// the sum is nonzero.
uint32_t bruteBest3(const std::vector<LapData>& laps) {
  uint32_t best = 0;
  for (size_t i = 0; i + 2 < laps.size(); i++) {
    if (!(laps[i].valid && laps[i + 1].valid && laps[i + 2].valid)) continue;
    uint32_t sum = (uint32_t)laps[i].lap_time_ms + laps[i + 1].lap_time_ms +
                   laps[i + 2].lap_time_ms;
    if (sum > 0 && (best == 0 || sum < best)) best = sum;
  }
  return best;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_best_empty_and_invalid(void) {
  TEST_Equal(LapStats::bestLapMs(nullptr, 0), 0);

  std::vector<LapData> laps;
  laps.push_back(lap(0));          // valid but zero time: ignored
  laps.push_back(lap(5000, false));  // invalid: ignored even if fast
  TEST_Equal(LapStats::bestLapMs(laps.data(), laps.size()), 0);

  laps.push_back(lap(1000));
  TEST_Equal(LapStats::bestLapMs(laps.data(), laps.size()), 1000);
}

void test_best_picks_minimum(void) {
  std::vector<LapData> laps = {lap(4000), lap(1200), lap(900), lap(1500)};
  TEST_Equal(LapStats::bestLapMs(laps.data(), laps.size()), 900);
}

void test_best3_requires_three(void) {
  std::vector<LapData> laps = {lap(1000), lap(1000)};
  TEST_Equal(LapStats::best3ConsecutiveMs(laps.data(), laps.size()), 0);
}

void test_best3_is_fastest_window_not_first(void) {
  // Window [100,100,100] then a much faster [50,50,50]: the answer must be
  // the LATER window.
  std::vector<LapData> laps = {
      lap(100), lap(100), lap(100), lap(200), lap(50), lap(50), lap(50)};
  TEST_Equal(LapStats::best3ConsecutiveMs(laps.data(), laps.size()), 150);
}

void test_best3_invalid_breaks_window(void) {
  // The invalid lap at index 2 breaks every window except [3,4,5].
  std::vector<LapData> laps = {
      lap(10), lap(10), lap(5, false), lap(10), lap(10), lap(10)};
  TEST_Equal(LapStats::best3ConsecutiveMs(laps.data(), laps.size()), 30);
}

void test_best3_zero_sum_windows_skipped(void) {
  // Pinned C++ semantics: a window is skipped only when its SUM is zero;
  // a zero-time lap inside a window still contributes the partial sum. (The
  // web UI instead filters zero laps out entirely — in practice the firmware
  // never emits a valid zero-time lap because the race start is always set,
  // so the two agree in operation.)
  std::vector<LapData> laps = {
      lap(0), lap(0), lap(0), lap(100), lap(100), lap(100)};
  // windows: [0,0,0]=0 skip; [0,0,100]=100; [0,100,100]=200; [100..]=300
  TEST_Equal(LapStats::best3ConsecutiveMs(laps.data(), laps.size()), 100);

  // All-zero laps => no window has a positive sum => 0.
  std::vector<LapData> zeros = {lap(0), lap(0), lap(0)};
  TEST_Equal(LapStats::best3ConsecutiveMs(zeros.data(), zeros.size()), 0);
}

void test_best3_property_vs_brute_force(void) {
  // Seeded pseudo-random fuzz: 300 trials, up to 120 laps, random validity
  // and times. The optimized sliding window must always match brute force.
  unsigned seed = 0xC0FFEEu;
  for (int trial = 0; trial < 300; trial++) {
    std::vector<LapData> laps;
    int n = (int)(1 + (seed = seed * 1103515245u + 12345u) % 120);
    for (int i = 0; i < n; i++) {
      seed = seed * 1103515245u + 12345u;
      bool valid = (seed % 10) != 0;  // ~10% invalid
      uint32_t ms = (seed >> 8) % 60000;  // 0..59999 ms
      laps.push_back(lap(ms, valid));
    }
    uint32_t got = LapStats::best3ConsecutiveMs(laps.data(), laps.size());
    uint32_t want = bruteBest3(laps);
    if (got != want) {
      TEST_FAIL_MESSAGE("best3 mismatch vs brute force");
    }
  }
  TEST_PASS();
}

void test_best_property_vs_brute_force(void) {
  unsigned seed = 0xBEEFu;
  for (int trial = 0; trial < 300; trial++) {
    std::vector<LapData> laps;
    int n = (int)((seed = seed * 1103515245u + 12345u) % 120);
    for (int i = 0; i < n; i++) {
      seed = seed * 1103515245u + 12345u;
      laps.push_back(lap((seed >> 8) % 60000, (seed % 10) != 0));
    }
    uint32_t best = 0;
    for (const auto& l : laps) {
      if (l.valid && l.lap_time_ms > 0 && (best == 0 || l.lap_time_ms < best))
        best = l.lap_time_ms;
    }
    uint32_t got = LapStats::bestLapMs(laps.data(), laps.size());
    if (got != best) {
      TEST_FAIL_MESSAGE("best mismatch vs reference");
    }
  }
  TEST_PASS();
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_best_empty_and_invalid);
  RUN_TEST(test_best_picks_minimum);
  RUN_TEST(test_best3_requires_three);
  RUN_TEST(test_best3_is_fastest_window_not_first);
  RUN_TEST(test_best3_invalid_breaks_window);
  RUN_TEST(test_best3_zero_sum_windows_skipped);
  RUN_TEST(test_best3_property_vs_brute_force);
  RUN_TEST(test_best_property_vs_brute_force);
  return UNITY_END();
}
