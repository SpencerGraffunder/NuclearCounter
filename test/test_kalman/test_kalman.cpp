// Unit tests for KalmanFilter (src/filters/kalman.cpp) — the RSSI smoother
// feeding the lap detector. Tests the .cpp directly (it is pure math).

#include <math.h>

// Unity 2.6 dropped the legacy TEST_* aliases; map the familiar short
// names onto the current TEST_ASSERT_* ones.
#define TEST_TRUE(cond)  TEST_ASSERT_TRUE(cond)
#define TEST_True(cond)  TEST_ASSERT_TRUE(cond)
#define TEST_FALSE(cond) TEST_ASSERT_FALSE(cond)
#define TEST_Equal(a, b) TEST_ASSERT_EQUAL(a, b)
#define TEST_False(cond) TEST_ASSERT_FALSE(cond)
#define TEST_EqualFloat(a, b, tol) TEST_ASSERT_FLOAT_WITHIN(tol, (a), (b))

#include <unity.h>

#include "../../src/filters/kalman.h"
#include "../../src/filters/kalman.cpp"

void setUp(void) {}
void tearDown(void) {}

void test_first_sample_initializes_to_measurement(void) {
  KalmanFilter f;
  TEST_TRUE(isnan(f.lastMeasurement()));  // uninitialized
  float x = f.filter(123);
  TEST_EqualFloat(x, 123.0f, 0.001f);
  TEST_EqualFloat(f.lastMeasurement(), 123.0f, 0.001f);
}

void test_constant_input_stays_constant(void) {
  KalmanFilter f;
  float x = 0;
  for (int i = 0; i < 100; i++) x = f.filter(80);
  TEST_EqualFloat(x, 80.0f, 0.5f);
}

void test_reduces_noise_variance(void) {
  // Alternating 100/120: every input step is 20 units. The firmware filter
  // config (Q=RSSI_FILTER_Q*0.01=20, R=RSSI_FILTER_R*0.0001=0.004, see
  // TimingCore's constructor) is very smooth, so the output's step-to-step
  // movement must be a small fraction of the input's, and the estimate must
  // settle on the 110 midpoint. (Measured: mean |dx| ~0.33, final ~110.06.)
  KalmanFilter f;
  f.setMeasurementNoise(20.0f);
  f.setProcessNoise(0.004f);
  float prev = 0, x = 0;
  double sum_dx = 0;
  int n_dx = 0;
  const int N = 200;
  for (int i = 0; i < N; i++) {
    x = f.filter((i % 2) ? 120 : 100);
    if (i > 10) {  // skip warm-up
      sum_dx += x > prev ? (double)(x - prev) : (double)(prev - x);
      n_dx++;
    }
    prev = x;
  }
  double mean_dx = sum_dx / n_dx;
  TEST_TRUE(mean_dx < 1.0);  // input moves 20 per sample
  TEST_EqualFloat(x, 110.0f, 2.0f);
}

void test_step_response_no_overshoot_converges(void) {
  // The firmware gain is small (K ~ 0.014), so convergence takes ~70 samples
  // per time constant — use 500. 0 < K < 1 => strictly monotone approach
  // that never overshoots the target.
  KalmanFilter f;
  f.setMeasurementNoise(20.0f);
  f.setProcessNoise(0.004f);
  for (int i = 0; i < 50; i++) f.filter(100);  // settle at 100
  float prev = 100.0f;
  for (int i = 0; i < 500; i++) {
    float x = f.filter(150);
    TEST_TRUE(x > prev);
    TEST_TRUE(x <= 150.0f + 0.001f);
    prev = x;
  }
  TEST_EqualFloat(prev, 150.0f, 0.5f);  // ~149.97 after 500 samples
}

void test_noise_settings_change_response(void) {
  // Higher measurement noise (Q) => lower gain => slower, smoother response.
  KalmanFilter fast, slow;
  fast.setMeasurementNoise(0.01f);   // trust measurements
  slow.setMeasurementNoise(100.0f);  // distrust measurements
  for (int i = 0; i < 50; i++) {
    fast.filter(100);
    slow.filter(100);
  }
  float f5 = fast.filter(200);
  float s5 = slow.filter(200);
  TEST_TRUE(f5 > s5);  // the fast filter moved further in one step
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_first_sample_initializes_to_measurement);
  RUN_TEST(test_constant_input_stays_constant);
  RUN_TEST(test_reduces_noise_variance);
  RUN_TEST(test_step_response_no_overshoot_converges);
  RUN_TEST(test_noise_settings_change_response);
  return UNITY_END();
}
