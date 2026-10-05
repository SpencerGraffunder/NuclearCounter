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
#define RSSI_HISTORY_ENABLED    1     // race-data export (10000 samples ~ 150KB heap, allocated at race start)
#define RSSI_HISTORY_SIZE       10000 // 20000 samples (400 s) at 50 Hz; 10000 keeps RAM ~150KB

// ---- Task scheduling (S3 dual-core: timing pinned to core 1) ----
#define TIMING_INTERVAL_MS  1
#define TIMING_PRIORITY     2
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

// ---- Core count (S3 = dual core; C3 builds don't use this config) ----
#ifndef CONFIG_SINGLE_CORE
#define CONFIG_SINGLE_CORE 0
#endif
