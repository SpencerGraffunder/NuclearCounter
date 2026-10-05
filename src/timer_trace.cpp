// Bench-only support for the TTRACE() stage timer (see src/config/config.h).
// The whole file compiles out of every production env: TIMER_TRACE is defined by
// [env:JITTER_C3] and [env:TRACE_C3] only.
#ifdef TIMER_TRACE

#include "config/config.h"
#include <Arduino.h>

TimerTraceRec g_timerTrace[TIMER_TRACE_MAX];
volatile int g_timerTraceN = 0;

void timerTraceRecord(const char *stage, uint32_t ms) {
  int i = g_timerTraceN;
  if (i < TIMER_TRACE_MAX) {
    g_timerTrace[i].stage = stage;   // string literal, lives in flash
    g_timerTrace[i].ms = ms;
    g_timerTraceN = i + 1;
  }
}

#endif
