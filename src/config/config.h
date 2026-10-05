/*
 * config.h — NuclearCounter integrated firmware (ported timing modules)
 *
 * Slim single-board config for the NuclearCounter S3 INTEGRATED build.
 * Pin definitions come from the per-env build_flags in platformio.ini
 * (same macros as the rest of the NC code: RSSI_PIN, PREVIOUS_BUTTON_PIN, etc.).
 * Constants below are the SFOS NuclearCounter-board values.
 */
#pragma once

// ---- Timing thresholds (0-255 RSSI scale, matching the SFOS web UI) ----
#define ENTER_RSSI        120
#define EXIT_RSSI         100
#define MIN_LAP_MS        0
#define MIN_LAP_TIME_MS   3000   // rejects threshold-bounce false laps

// ---- Kalman filter (values are 100x / 10000x scaled integers in SFOS) ----
#define RSSI_FILTER_Q     2000   // 0.01 scale  -> 20.0 measurement noise
#define RSSI_FILTER_R     40     // 0.0001 scale -> 0.004 process noise

// ---- Lap / history storage ----
#define MAX_LAPS_STORED         50
#define RSSI_SAMPLE_INTERVAL_MS 20    // 50 Hz
#define RSSI_HISTORY_ENABLED    1     // race-data export (heap buffer allocated at race start)
// RSSISample is 8 bytes (uint32 + uint8 padded), so the buffer is size*8.
// Measured on the C3 board: ~191 KB free heap before the AP, ~100 KB after the
// WiFi stack comes up. The S3 has more headroom, so the C3 gets a smaller
// buffer: 6000 samples = 48 KB, leaving ~52 KB for the WiFi/LWIP runtime.
// At 50 Hz that is 120 s of race data on the C3 (400 s on the S3).
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(ARDUINO_ESP32C3_DEV)
#define RSSI_HISTORY_SIZE       6000
#else
#define RSSI_HISTORY_SIZE       10000
#endif

// ---- Task scheduling (S3 dual-core: timing pinned to core 1) ----
#define TIMING_INTERVAL_MS  1
// Overridable from a build flag (bench A/B only; no production env overrides it).
// On the single-core C3 the lwIP TCP/IP task runs at 18 and the Wi-Fi driver above
// that, so priority 2 loses the core to HTTP traffic. MEASURED: raising it to 20 is
// WORSE for the real 4 Hz UI load (880 -> 770 samples/s) because the timing task then
// preempts lwIP mid-packet and the whole exchange takes longer; the ~400 ms stalls
// happen outside the critical section, so priority is not the lever. Keep 2.
#ifndef TIMING_PRIORITY
#define TIMING_PRIORITY     2
#endif
#define WEB_PRIORITY        1

// ---- RX5808 frequency range (6 bands x 8 channels, 25 MHz spacing) ----
#define MIN_FREQ            5645
#define MAX_FREQ            5945
#define DEFAULT_FREQ        5800
#define FREQ_BANDS          6
#define FREQ_CHANNELS       8

// ---- Web / mDNS ----
#define WEB_SERVER_PORT     80
#define MDNS_HOSTNAME       "nuclearcounter"

// ---- Feature switches (not present on this hardware) ----
#define ENABLE_STATUS_LED       0
#define ENABLE_LCD_UI           0
#define ENABLE_POWER_BUTTON     0
#define ENABLE_AUDIO            0
#define ENABLE_BATTERY_MONITOR  0

// ---- Core count ----
// RX5808.cpp's scan-task pinning guard reads this. Single-core targets (C3)
// have no core 1, so it must be 1 there — pinning to core 1 on a single-core
// build fails at runtime. Made chip-aware so a C3 env cannot accidentally
// inherit the S3 value (RX5808.cpp now includes this header so the guard sees
// it regardless of the env's build_flags).
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(ARDUINO_ESP32C3_DEV) || defined(CONFIG_FREERTOS_UNICORE)
#undef CONFIG_SINGLE_CORE
#define CONFIG_SINGLE_CORE 1
#else
#ifndef CONFIG_SINGLE_CORE
#define CONFIG_SINGLE_CORE 0
#endif
#endif
