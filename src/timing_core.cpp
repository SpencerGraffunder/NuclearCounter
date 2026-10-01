#include "timing_core.h"
#include "RX5808.h"

// RX5808 timing constants
#define RX5808_MIN_TUNETIME 35    // ms - wait after freq change before reading RSSI
#define RX5808_MIN_BUSTIME  30    // ms - minimum time between freq changes

// Static variable to track last RX5808 bus access time (shared across all instances)
static uint32_t lastRX5808BusTimeMs = 0;

// RX5808 Frequency Table (in MHz)
// Bands: A, B, E, F, R, L (indices 0-5)
// Channels: 1-8 (indices 0-7)
static const uint16_t freqTable[6][8] = {
  // Band A (Boscam A)
  {5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725},
  // Band B (Boscam B)
  {5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866},
  // Band E (Boscam E / DJI)
  {5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945},
  // Band F (Fatshark / NexWave)
  {5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880},
  // Band R (Raceband)
  {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917},
  // Band L (Low Race)
  {5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621}
};

// Helper function to get frequency from band/channel
static uint16_t getFrequencyFromBandChannel(uint8_t band, uint8_t channel) {
  if (band >= 6 || channel >= 8) return 5865;  // Default to A1 if invalid
  return freqTable[band][channel];
}

// Helper function to get band/channel from frequency (finds closest match)
static void getBandChannelFromFrequency(uint16_t freq, uint8_t& band, uint8_t& channel) {
  uint16_t min_diff = 65535;
  band = 0;
  channel = 0;
  
  for (uint8_t b = 0; b < 6; b++) {
    for (uint8_t c = 0; c < 8; c++) {
      uint16_t diff = abs((int)freqTable[b][c] - (int)freq);
      if (diff < min_diff) {
        min_diff = diff;
        band = b;
        channel = c;
      }
    }
  }
}


TimingCore::TimingCore() {
  // Initialize state
  memset(&state, 0, sizeof(state));
  state.enter_rssi = ENTER_RSSI;
  state.exit_rssi = EXIT_RSSI;
  state.frequency_mhz = DEFAULT_FREQ;
  state.nadir_rssi = 255;  // Initialize to max value
  state.pass_rssi_nadir = 255;
  state.last_rssi = 0;
  state.rssi_change = 0;
  
  // Initialize buffers
  memset(lap_buffer, 0, sizeof(lap_buffer));
  memset(peak_buffer, 0, sizeof(peak_buffer));
  memset(nadir_buffer, 0, sizeof(nadir_buffer));
  
  lap_write_index = 0;
  lap_read_index = 0;
  debug_enabled = false; // Default to no debug output
  recent_freq_change = false;
  freq_change_time = 0;
  current_band = 0;  // Default to band A
  current_channel = 0;  // Default to channel 1
  
  // Initialize Kalman filter with configuration values
  rssi_filter.setMeasurementNoise(RSSI_FILTER_Q * 0.01f);
  rssi_filter.setProcessNoise(RSSI_FILTER_R * 0.0001f);
  
  // Initialize peak tracking
  rssi_peak = 0;
  rssi_peak_time_ms = 0;
  lap_start_time_ms = 0;
  race_start_time_ms = 0;
  min_lap_ms = MIN_LAP_MS;
  
  // RX5808 instance is bound via setRX5808() before begin()
  _rx5808 = nullptr;
  
  // Initialize extremum tracking
  peak_write_index = 0;
  peak_read_index = 0;
  nadir_write_index = 0;
  nadir_read_index = 0;

  memset(&current_peak, 0, sizeof(current_peak));
  memset(&current_nadir, 0, sizeof(current_nadir));
  current_nadir.rssi = 255;

  // Initialize RSSI history buffering
  // CRITICAL: Defer allocation until after WiFi is initialized to prevent heap fragmentation
  // WiFi needs contiguous memory blocks, and allocating 150KB early can cause crashes
#if RSSI_HISTORY_ENABLED
  rssi_history_buffer = nullptr;  // Allocated via allocateRSSIHistory() (background task / race start)
  rssi_history_write_index = 0;
  rssi_history_count = 0;
  last_rssi_sample_time = 0;
#endif

  // Initialize FreeRTOS objects
  timing_task_handle = nullptr;
  timing_mutex = xSemaphoreCreateMutex();
}

void TimingCore::begin() {
  if (debug_enabled) {
    Serial.println("TimingCore: Initializing...");
  }
  
  // Setup RSSI input pin (RF pin handling is owned by the NC RX5808 instance)
  pinMode(RSSI_PIN, INPUT);
  
  // CRITICAL: Configure ADC attenuation for full 0-3.3V range
  // RX5808 RSSI output is 0-3.3V, so we need 11dB attenuation
  // Without this, the ADC defaults to a lower voltage range and saturates
  analogSetAttenuation(ADC_11db);  // Set global attenuation to 11dB (0-3.3V)
  
  if (debug_enabled) {
    Serial.println("ADC configured for 0-3.3V range (11dB attenuation)");
  }
  
  // Set default frequency (delegates to the NC RX5808 instance)
  setRX5808Frequency(state.frequency_mhz);
  
  // Initialize Kalman filter with some test readings to stabilize it
  for (int i = 0; i < 10; i++) {
    uint8_t raw_rssi = readRawRSSI();
    rssi_filter.filter(raw_rssi, 0);
  }
  
  // Note: RSSI calibration will be done after debug mode is set
  
  // RSSI history buffer allocation is deferred until first use (lazy allocation)
  // This prevents heap fragmentation during WiFi initialization
  // The buffer will be allocated when the first race starts (after WiFi is ready)
  
  // Create timing task with appropriate core pinning
  // ESP32-C3/C6 (single core): Use xTaskCreate (no core pinning)
  // ESP32 dual-core: Pin to Core 1 (away from WiFi/Arduino on Core 0)
#if defined(CONFIG_IDF_TARGET_ESP32C6) || defined(ARDUINO_ESP32C3_DEV) || defined(CONFIG_IDF_TARGET_ESP32C3)
  xTaskCreate(timingTask, "TimingTask", 4096, this, TIMING_PRIORITY, &timing_task_handle);
#else
  xTaskCreatePinnedToCore(timingTask, "TimingTask", 4096, this, TIMING_PRIORITY, &timing_task_handle, 1);
#endif
  
  // Do NOT activate here - will be activated after mode-specific initialization
  // state.activated = true;  // REMOVED - see main.cpp setup() for activation timing
  
  if (debug_enabled) {
    Serial.println("TimingCore: Ready (inactive until mode init)");
  }
}

void TimingCore::process() {
  // For ESP32-C3, timing is handled by the dedicated task
  // This method is kept for compatibility but does minimal work
  if (!state.activated) {
    return;
  }
  
  // Just yield to allow other tasks to run
  vTaskDelay(pdMS_TO_TICKS(1));
}

// FreeRTOS task for timing processing (ESP32-C3 single core)
void TimingCore::timingTask(void* parameter) {
  TimingCore* core = static_cast<TimingCore*>(parameter);
  
  // Performance monitoring variables
  uint32_t loop_count = 0;
  uint32_t last_perf_time = 0;
  uint32_t min_loop_time = UINT32_MAX;
  uint32_t max_loop_time = 0;
  uint32_t total_loop_time = 0;
  
  while (true) {
    if (!core->state.activated) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    
    uint32_t loop_start = micros();
    static uint32_t last_process_time = 0;
    uint32_t current_time = millis();
    
    // Limit processing to configured interval
    if (current_time - last_process_time < TIMING_INTERVAL_MS) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    
    // Take mutex for thread safety
    if (xSemaphoreTake(core->timing_mutex, portMAX_DELAY)) {
      // Read and filter RSSI
      uint8_t raw_rssi = core->readRawRSSI();
      
      uint8_t filtered_rssi = core->filterRSSI(raw_rssi);
      core->state.current_rssi = filtered_rssi;

      // Buffer RSSI sample for race data export (50Hz)
      // Lazy allocation: Allocate buffer on first use (after WiFi is initialized)
#if RSSI_HISTORY_ENABLED
      if (core->race_start_time_ms > 0 &&
          (current_time - core->last_rssi_sample_time >= RSSI_SAMPLE_INTERVAL_MS)) {
        // The RSSI history buffer is allocated by allocateRSSIHistory() —
        // from a background task after boot and at race start — never in
        // here: a ~150KB malloc/memset must not run inside this mutex's
        // critical section (it held the lock for seconds and stalled every
        // setActivated()/state change until fixed).
        if (core->rssi_history_buffer) {
          uint32_t race_time = current_time - core->race_start_time_ms;
          core->rssi_history_buffer[core->rssi_history_write_index].timestamp_ms = race_time;
          core->rssi_history_buffer[core->rssi_history_write_index].rssi = filtered_rssi;
          core->rssi_history_write_index = (core->rssi_history_write_index + 1) % RSSI_HISTORY_SIZE;
          if (core->rssi_history_count < RSSI_HISTORY_SIZE) {
            core->rssi_history_count++;
          }
          core->last_rssi_sample_time = current_time;
        }
      }
#endif

      // Update nadir tracking (always track, regardless of crossing state)
      if (filtered_rssi < core->state.nadir_rssi) {
        core->state.nadir_rssi = filtered_rssi;
      }
      if (filtered_rssi < core->state.pass_rssi_nadir) {
        core->state.pass_rssi_nadir = filtered_rssi;
      }

      // Process extremums for marshal mode history
      core->processExtremums(current_time, filtered_rssi);
      
      // Check if minimum lap time has elapsed (prevents peak capture too soon)
      bool can_capture_peak = true;
      if (core->min_lap_ms > 0 && core->lap_start_time_ms > 0) {
        uint32_t time_since_lap_start = current_time - core->lap_start_time_ms;
        if (time_since_lap_start < core->min_lap_ms) {
          can_capture_peak = false;
        }
      }
      
      // Capture peak only when RSSI >= enterRssi and min lap time has elapsed
      if (can_capture_peak && filtered_rssi >= core->state.enter_rssi) {
        // Check if RSSI is greater than the previous detected peak
        if (filtered_rssi > core->rssi_peak) {
          core->rssi_peak = filtered_rssi;
          core->rssi_peak_time_ms = current_time;
          core->state.peak_rssi = filtered_rssi;  // Update state for compatibility
        }
      }
      
      // Check if peak is captured (RSSI dropped below peak AND below exit threshold)
      bool peak_captured = (filtered_rssi < core->rssi_peak) && (filtered_rssi < core->state.exit_rssi);
      
      // Update crossing state based on whether we're capturing a peak
      bool was_crossing = core->state.crossing_active;
      core->state.crossing_active = (can_capture_peak && filtered_rssi >= core->state.enter_rssi);
      
      // Handle crossing state changes
      if (was_crossing != core->state.crossing_active) {
        if (core->state.crossing_active) {
          // Starting a crossing
          core->state.crossing_start = current_time;
          if (core->debug_enabled) {
            Serial.printf("Crossing started - RSSI: %d\n", filtered_rssi);
          }
        } else {
          // Ending a crossing
          if (core->debug_enabled) {
            Serial.printf("Crossing ended - RSSI: %d\n", filtered_rssi);
          }
        }
        
        // Notify callback if registered  
        if (core->crossing_callback) {
          core->crossing_callback(core->state.crossing_active, filtered_rssi);
        }
      }
      
      // Check if lap should be recorded (peak captured and enough time since last lap)
      if (peak_captured && core->rssi_peak > 0) {
        uint32_t time_since_last_lap = (core->state.last_lap_time > 0) ? 
                                       (current_time - core->state.last_lap_time) : 
                                       UINT32_MAX;
        
        // Require minimum time between laps
        if (time_since_last_lap >= MIN_LAP_TIME_MS) {
          // Valid lap - record it using peak timestamp
          core->recordLap(core->rssi_peak_time_ms, core->rssi_peak);
          // Reset pass nadir after lap recorded
          core->state.pass_rssi_nadir = 255;
          
          if (core->debug_enabled) {
            Serial.printf("Lap recorded - Peak RSSI: %d, Time since last: %dms\n", 
                         core->rssi_peak, time_since_last_lap);
          }
        } else {
          // Lap rejected - too soon after last lap
          // Only print rejection message once per second to avoid spam
          static uint32_t last_rejection_print = 0;
          if (core->debug_enabled && (current_time - last_rejection_print >= 1000)) {
            Serial.printf("Lap rejected - Too soon (only %dms since last lap, need %dms)\n", 
                         time_since_last_lap, MIN_LAP_TIME_MS);
            last_rejection_print = current_time;
          }
        }
      }
      
      last_process_time = current_time;
      xSemaphoreGive(core->timing_mutex);
    }
    
    // Yield to other tasks (especially main loop for serial processing on single-core ESP32-C3)
    taskYIELD();
    
    // Performance monitoring
    uint32_t loop_end = micros();
    uint32_t loop_time = loop_end - loop_start;
    
    loop_count++;
    if (loop_time < min_loop_time) min_loop_time = loop_time;
    if (loop_time > max_loop_time) max_loop_time = loop_time;
    total_loop_time += loop_time;
    
    // Report performance every 5 seconds - only in debug mode
    uint32_t now = millis();
    if (now - last_perf_time >= 5000) {
      if (core->debug_enabled) {
        uint32_t avg_loop_time = total_loop_time / loop_count;
        uint32_t loops_per_second = (loop_count * 1000) / (now - last_perf_time);
        
        Serial.printf("[TimingPerf] Loops/sec: %d, Avg: %dus, Min: %dus, Max: %dus\n", 
                      loops_per_second, avg_loop_time, min_loop_time, max_loop_time);
      }
      
      // Reset counters
      loop_count = 0;
      last_perf_time = now;
      min_loop_time = UINT32_MAX;
      max_loop_time = 0;
      total_loop_time = 0;
    }
    
    // No delay needed here - rate limiting is handled by TIMING_INTERVAL_MS check at top of loop
    // The check at line 233 already ensures we don't process more than once per TIMING_INTERVAL_MS
  }
}

uint8_t TimingCore::readRawRSSI() {
  // Check if frequency was recently changed - RSSI is unstable during tuning
  // Wait for tuning to complete before reading (same as RotorHazard)
  if (recent_freq_change) {
    uint32_t timeVal = millis() - freq_change_time;
    if (timeVal < RX5808_MIN_TUNETIME) {
      delay(RX5808_MIN_TUNETIME - timeVal);  // wait until after-tune-delay time is fulfilled
    }
    recent_freq_change = false;  // don't need to check again until next freq change
  }
  
  // Read 12-bit ADC value (0-4095 on ESP32)
  uint16_t adc_value = analogRead(RSSI_PIN);
  
  // RX5808 RSSI typically outputs 0-2V (not full 0-3.3V range)
  // Clamp to 2047 (half of 12-bit range = ~2V @ 3.3V reference)
  // This prevents noise in the unused upper range from affecting readings
  if (adc_value > 2047) {
    adc_value = 2047;
  }
  
  // Rescale from 0-2047 to 0-255 (divide by 8)
  // This gives us the standard 0-255 RSSI range used by lap timing systems
  return adc_value >> 3;
}

uint8_t TimingCore::filterRSSI(uint8_t raw_rssi) {
  // Kalman filter for RSSI smoothing
  float filtered = rssi_filter.filter(raw_rssi, 0);
  return (uint8_t)round(filtered);
}

bool TimingCore::detectCrossing(uint8_t filtered_rssi) {
  // Dual threshold-based crossing detection
  // Crossing is active when RSSI >= enter threshold
  return filtered_rssi >= state.enter_rssi;
}

void TimingCore::recordLap(uint32_t timestamp, uint8_t peak_rssi) {
  LapData& lap = lap_buffer[lap_write_index];
  
  // Use peak timestamp for lap timing
  lap.timestamp_ms = timestamp;
  
  // Calculate lap time: first lap uses race start time, others use previous lap start time
  if (state.lap_count == 0) {
    // First lap - use race start time
    lap.lap_time_ms = (race_start_time_ms > 0) ? 
                     (timestamp - race_start_time_ms) : 0;
  } else {
    // Subsequent laps - use previous lap start time
    lap.lap_time_ms = (lap_start_time_ms > 0) ? 
                     (timestamp - lap_start_time_ms) : 0;
  }
  
  lap.rssi_peak = peak_rssi;
  lap.pilot_id = 0; // Single pilot for now
  lap.valid = true;
  
  // Update state
  state.last_lap_time = timestamp;
  state.lap_count++;
  lap_start_time_ms = timestamp;  // Update lap start time for next lap
  
  // Advance write index
  lap_write_index = (lap_write_index + 1) % MAX_LAPS_STORED;
  
  // Reset peak tracking for next lap
  rssi_peak = 0;
  rssi_peak_time_ms = 0;
  state.peak_rssi = 0;
  
        // Debug output disabled to avoid interfering with serial protocol
        // DEBUG_PRINT("Lap recorded: ");
        // DEBUG_PRINT(state.lap_count);
        // DEBUG_PRINT(", Time: ");
        // DEBUG_PRINT(lap.lap_time_ms);
        // DEBUG_PRINT("ms, Peak: ");
        // DEBUG_PRINTLN(peak_rssi);
  
  // Notify callback if registered
  if (lap_callback) {
    lap_callback(lap);
  }
}

// Extremum tracking implementation (for marshal mode)
// Based on RotorHazard reference implementation in RssiNode.cpp
void TimingCore::processExtremums(uint32_t timestamp, uint8_t filtered_rssi) {
  // Detect RSSI change direction
  const int rssiChange = filtered_rssi - state.last_rssi;
  
  if (rssiChange > 0) {
    // RSSI is rising
    // Must buffer latest peak to prevent losing it (overwriting any unsent peak)
    bufferPeak(current_peak);
    
    // Start tracking new peak with current RSSI value
    current_peak.rssi = filtered_rssi;
    current_peak.first_time = timestamp;
    current_peak.duration = 0;
    current_peak.valid = true;
    
    // If RSSI was falling or unchanged, but it's rising now, we found a nadir
    if (state.rssi_change <= 0 && current_nadir.valid) {
      // Finalize and buffer the nadir we were tracking
      finalizeNadir(timestamp);
    }
    
  } else if (rssiChange < 0) {
    // RSSI is falling
    // Must buffer latest nadir to prevent losing it (overwriting any unsent nadir)
    bufferNadir(current_nadir);
    
    // Start tracking new nadir with current RSSI value
    current_nadir.rssi = filtered_rssi;
    current_nadir.first_time = timestamp;
    current_nadir.duration = 0;
    current_nadir.valid = true;
    
    // If RSSI was rising or unchanged, but it's falling now, we found a peak
    if (state.rssi_change >= 0 && current_peak.valid) {
      // Finalize and buffer the peak we were tracking
      finalizePeak(timestamp);
    }
    
  } else {
    // RSSI is unchanged
    if (filtered_rssi == current_peak.rssi && current_peak.valid) {
      // Still at peak - extend duration
      uint32_t duration = timestamp - current_peak.first_time;
      current_peak.duration = (duration > 0xFFFF) ? 0xFFFF : duration;
      
      // If peak duration maxed out, buffer it and start a new one
      if (current_peak.duration == 0xFFFF) {
        bufferPeak(current_peak);
        current_peak.rssi = filtered_rssi;
        current_peak.first_time = timestamp;
        current_peak.duration = 0;
        current_peak.valid = true;
      }
    } else if (filtered_rssi == current_nadir.rssi && current_nadir.valid) {
      // Still at nadir - extend duration
      uint32_t duration = timestamp - current_nadir.first_time;
      current_nadir.duration = (duration > 0xFFFF) ? 0xFFFF : duration;
      
      // If nadir duration maxed out, buffer it and start a new one
      if (current_nadir.duration == 0xFFFF) {
        bufferNadir(current_nadir);
        current_nadir.rssi = filtered_rssi;
        current_nadir.first_time = timestamp;
        current_nadir.duration = 0;
        current_nadir.valid = true;
      }
    }
  }
  
  // Update state for next iteration
  state.last_rssi = filtered_rssi;
  
  // Clamp rssiChange to prevent overflow (like RotorHazard does)
  if (rssiChange > 127) {
    state.rssi_change = 127;
  } else if (rssiChange < -127) {
    state.rssi_change = -127;
  } else {
    state.rssi_change = rssiChange;
  }
}

void TimingCore::finalizePeak(uint32_t timestamp) {
  if (current_peak.valid && current_peak.rssi > 0) {
    // Calculate final duration
    uint32_t duration = timestamp - current_peak.first_time;
    current_peak.duration = (duration > 0xFFFF) ? 0xFFFF : duration;
    
    // Buffer the completed peak
    bufferPeak(current_peak);
  }
  
  // Reset for next peak
  memset(&current_peak, 0, sizeof(current_peak));
}

void TimingCore::finalizeNadir(uint32_t timestamp) {
  if (current_nadir.valid && current_nadir.rssi < 255) {
    // Calculate final duration
    uint32_t duration = timestamp - current_nadir.first_time;
    current_nadir.duration = (duration > 0xFFFF) ? 0xFFFF : duration;
    
    // Buffer the completed nadir
    bufferNadir(current_nadir);
  }
  
  // Reset for next nadir
  memset(&current_nadir, 0, sizeof(current_nadir));
  current_nadir.rssi = 255;
}

void TimingCore::bufferPeak(const Extremum& peak) {
  // Only buffer valid peaks (RotorHazard checks validity before buffering)
  if (!peak.valid || peak.rssi == 0) {
    return;
  }
  
  peak_buffer[peak_write_index] = peak;
  peak_write_index = (peak_write_index + 1) & (EXTREMUM_BUFFER_SIZE - 1);
  
  // Check for overflow (if write catches read, drop oldest)
  if (peak_write_index == peak_read_index) {
    peak_read_index = (peak_read_index + 1) & (EXTREMUM_BUFFER_SIZE - 1);
  }
}

void TimingCore::bufferNadir(const Extremum& nadir) {
  // Only buffer valid nadirs (RotorHazard checks validity before buffering)
  if (!nadir.valid || nadir.rssi == 255) {
    return;
  }
  
  nadir_buffer[nadir_write_index] = nadir;
  nadir_write_index = (nadir_write_index + 1) & (EXTREMUM_BUFFER_SIZE - 1);
  
  // Check for overflow (if write catches read, drop oldest)
  if (nadir_write_index == nadir_read_index) {
    nadir_read_index = (nadir_read_index + 1) & (EXTREMUM_BUFFER_SIZE - 1);
  }
}

void TimingCore::setRX5808Frequency(uint16_t freq_mhz) {
  // The NC RX5808 instance owns the RF pins; it must be bound first
  if (!_rx5808) {
    if (debug_enabled) {
      Serial.println("TimingCore: no RX5808 instance bound (call setRX5808() before begin())");
    }
    return;
  }
  
  // Check if enough time has elapsed since last RX5808 bus access
  // This prevents bus conflicts and ensures reliable communication
  uint32_t timeVal = millis() - lastRX5808BusTimeMs;
  if (timeVal < RX5808_MIN_BUSTIME) {
    delay(RX5808_MIN_BUSTIME - timeVal);  // wait until after-bus-delay time is fulfilled
  }
  
  if (freq_mhz < MIN_FREQ || freq_mhz > MAX_FREQ) {
    if (debug_enabled) {
      Serial.printf("Invalid frequency: %d MHz (valid range: %d-%d)\n", freq_mhz, MIN_FREQ, MAX_FREQ);
    }
    return;
  }
  
  _rx5808->setFrequency(freq_mhz);
  
  state.frequency_mhz = freq_mhz;
  
  // Mark frequency change time to prevent unstable RSSI reads
  recent_freq_change = true;
  freq_change_time = millis();
  lastRX5808BusTimeMs = freq_change_time;  // Track last bus access time
}

// Public interface methods (thread-safe for ESP32-C3)
void TimingCore::setFrequency(uint16_t freq_mhz) {
  if (xSemaphoreTake(timing_mutex, portMAX_DELAY)) {
    setRX5808Frequency(freq_mhz);
    xSemaphoreGive(timing_mutex);
  }
}

void TimingCore::setEnterRssi(uint8_t enter_rssi) {
  if (xSemaphoreTake(timing_mutex, portMAX_DELAY)) {
    state.enter_rssi = enter_rssi;
    xSemaphoreGive(timing_mutex);
  }
}

void TimingCore::setExitRssi(uint8_t exit_rssi) {
  if (xSemaphoreTake(timing_mutex, portMAX_DELAY)) {
    state.exit_rssi = exit_rssi;
    xSemaphoreGive(timing_mutex);
  }
}

void TimingCore::setThreshold(uint8_t threshold) {
  // DEPRECATED: For backward compatibility, set both thresholds
  // Set enter threshold to provided value, exit to 20 below
  if (xSemaphoreTake(timing_mutex, portMAX_DELAY)) {
    state.enter_rssi = threshold;
    state.exit_rssi = (threshold > 20) ? (threshold - 20) : threshold;
    xSemaphoreGive(timing_mutex);
  }
}

void TimingCore::setMinLapMs(uint32_t min_lap_ms) {
  if (xSemaphoreTake(timing_mutex, portMAX_DELAY)) {
    this->min_lap_ms = min_lap_ms;
    xSemaphoreGive(timing_mutex);
  }
}

bool TimingCore::pauseTemporarily(uint32_t pauseMs) {
  // Temporarily pause timing core to give other tasks (like file serving) full CPU
  // Returns the previous activation state so it can be restored
  bool wasActive = false;
  if (xSemaphoreTake(timing_mutex, portMAX_DELAY)) {
    wasActive = state.activated;
    if (wasActive) {
      state.activated = false;
    }
    xSemaphoreGive(timing_mutex);
    // Give timing task time to see the pause and yield
    delay(10);
  }
  return wasActive;
}

void TimingCore::resumeFromPause(bool wasActive) {
  // Restore timing core to previous activation state
  if (xSemaphoreTake(timing_mutex, portMAX_DELAY)) {
    state.activated = wasActive;
    xSemaphoreGive(timing_mutex);
  }
}

void TimingCore::setActivated(bool active) {
  // Use a bounded wait, NOT portMAX_DELAY: the timing task can hold this
  // mutex for a long time (RSSI buffer malloc / tune-wait), and a blocking
  // take here hung setup() forever, killing the Arduino loop (buttons dead,
  // menu frozen). A short timeout lets boot always complete; the flag is
  // (re)set by the caller or the task is already running.
  if (xSemaphoreTake(timing_mutex, pdMS_TO_TICKS(100))) {
    state.activated = active;
    if (active && race_start_time_ms == 0) {
      // Set race start time when first activated
      race_start_time_ms = millis();
    }
    // Timing activated/deactivated
    xSemaphoreGive(timing_mutex);
  }
}

void TimingCore::reset() {
  if (xSemaphoreTake(timing_mutex, portMAX_DELAY)) {
    state.lap_count = 0;
    state.last_lap_time = 0;
    state.peak_rssi = 0;
    state.nadir_rssi = 255;
    state.pass_rssi_nadir = 255;
    state.crossing_active = false;
    state.last_rssi = 0;
    state.rssi_change = 0;
    
    // Reset peak tracking
    rssi_peak = 0;
    rssi_peak_time_ms = 0;
    lap_start_time_ms = 0;
    race_start_time_ms = 0;
    
    // Clear lap buffer
    memset(lap_buffer, 0, sizeof(lap_buffer));
    lap_write_index = 0;
    lap_read_index = 0;
    
    // Clear extremum buffers
    memset(peak_buffer, 0, sizeof(peak_buffer));
    memset(nadir_buffer, 0, sizeof(nadir_buffer));
    peak_write_index = 0;
    peak_read_index = 0;
    nadir_write_index = 0;
    nadir_read_index = 0;
    
    memset(&current_peak, 0, sizeof(current_peak));
    memset(&current_nadir, 0, sizeof(current_nadir));
    current_nadir.rssi = 255;
    
    // Timing reset
    xSemaphoreGive(timing_mutex);
  }
}

bool TimingCore::hasNewLap() {
  return lap_read_index != lap_write_index;
}

LapData TimingCore::getNextLap() {
  if (!hasNewLap()) {
    LapData empty = {0};
    return empty;
  }
  
  LapData lap = lap_buffer[lap_read_index];
  lap_read_index = (lap_read_index + 1) % MAX_LAPS_STORED;
  return lap;
}

LapData TimingCore::getLastLap() {
  if (state.lap_count == 0) {
    LapData empty = {0};
    return empty;
  }
  
  uint8_t last_index = (lap_write_index - 1 + MAX_LAPS_STORED) % MAX_LAPS_STORED;
  return lap_buffer[last_index];
}

uint8_t TimingCore::getAvailableLaps() {
  return (lap_write_index - lap_read_index + MAX_LAPS_STORED) % MAX_LAPS_STORED;
}

// Extremum data access methods (for marshal mode)
bool TimingCore::hasPendingPeak() {
  return peak_read_index != peak_write_index;
}

bool TimingCore::hasPendingNadir() {
  return nadir_read_index != nadir_write_index;
}

Extremum TimingCore::getNextPeak() {
  if (!hasPendingPeak()) {
    Extremum empty = {0};
    return empty;
  }
  
  Extremum peak = peak_buffer[peak_read_index];
  peak_read_index = (peak_read_index + 1) & (EXTREMUM_BUFFER_SIZE - 1);
  return peak;
}

Extremum TimingCore::getNextNadir() {
  if (!hasPendingNadir()) {
    Extremum empty = {255, 0, 0, false};
    return empty;
  }
  
  Extremum nadir = nadir_buffer[nadir_read_index];
  nadir_read_index = (nadir_read_index + 1) & (EXTREMUM_BUFFER_SIZE - 1);
  return nadir;
}

Extremum TimingCore::peekNextPeak() const {
  if (peak_read_index == peak_write_index) {
    Extremum empty = {0, 0, 0, false};
    return empty;
  }
  
  return peak_buffer[peak_read_index];
}

Extremum TimingCore::peekNextNadir() const {
  if (nadir_read_index == nadir_write_index) {
    Extremum empty = {255, 0, 0, false};
    return empty;
  }
  
  return nadir_buffer[nadir_read_index];
}

uint8_t TimingCore::getNadirRSSI() const {
  uint8_t nadir = 255;
  if (xSemaphoreTake(timing_mutex, portMAX_DELAY)) {
    nadir = state.nadir_rssi;
    xSemaphoreGive(timing_mutex);
  }
  return nadir;
}

uint8_t TimingCore::getPassNadirRSSI() const {
  uint8_t nadir = 255;
  if (xSemaphoreTake(timing_mutex, portMAX_DELAY)) {
    nadir = state.pass_rssi_nadir;
    xSemaphoreGive(timing_mutex);
  }
  return nadir;
}

// Thread-safe state access methods
TimingState TimingCore::getState() const {
  TimingState current_state;
  if (xSemaphoreTake(timing_mutex, pdMS_TO_TICKS(10))) {
    current_state = state;
    xSemaphoreGive(timing_mutex);
  } else {
    // If we can't get the mutex quickly, return a zeroed state
    memset(&current_state, 0, sizeof(current_state));
  }
  return current_state;
}

uint8_t TimingCore::getCurrentRSSI() const {
  uint8_t rssi = 0;
  if (xSemaphoreTake(timing_mutex, portMAX_DELAY)) {
    rssi = state.current_rssi;
    xSemaphoreGive(timing_mutex);
  }
  return rssi;
}

uint8_t TimingCore::getPeakRSSI() const {
  uint8_t peak = 0;
  if (xSemaphoreTake(timing_mutex, portMAX_DELAY)) {
    peak = state.peak_rssi;
    xSemaphoreGive(timing_mutex);
  }
  return peak;
}

uint16_t TimingCore::getLapCount() const {
  uint16_t count = 0;
  if (xSemaphoreTake(timing_mutex, pdMS_TO_TICKS(10))) {
    count = state.lap_count;
    xSemaphoreGive(timing_mutex);
  }
  return count;
}

bool TimingCore::isActivated() const {
  bool activated = false;
  if (xSemaphoreTake(timing_mutex, pdMS_TO_TICKS(10))) {
    activated = state.activated;
    xSemaphoreGive(timing_mutex);
  }
  return activated;
}

bool TimingCore::isCrossing() const {
  bool crossing = false;
  if (xSemaphoreTake(timing_mutex, pdMS_TO_TICKS(10))) {
    crossing = state.crossing_active;
    xSemaphoreGive(timing_mutex);
  }
  return crossing;
}

void TimingCore::setDebugMode(bool debug_enabled) {
  this->debug_enabled = debug_enabled;
}

void TimingCore::setRX5808Settings(uint8_t band, uint8_t channel) {
  if (band >= 6 || channel >= 8) return;  // Validate input
  
  if (xSemaphoreTake(timing_mutex, portMAX_DELAY)) {
    current_band = band;
    current_channel = channel;
    uint16_t freq = getFrequencyFromBandChannel(band, channel);
    setRX5808Frequency(freq);
    xSemaphoreGive(timing_mutex);
  }
}

// Configuration getters
uint8_t TimingCore::getEnterRssi() const {
  return state.enter_rssi;
}

uint8_t TimingCore::getExitRssi() const {
  return state.exit_rssi;
}

uint8_t TimingCore::getThreshold() const {
  // DEPRECATED: Returns enter_rssi for backward compatibility
  return state.enter_rssi;
}

uint32_t TimingCore::getMinLapMs() const {
  return min_lap_ms;
}

uint16_t TimingCore::getCurrentFrequency() const {
  return state.frequency_mhz;
}

void TimingCore::getRX5808Settings(uint8_t& band, uint8_t& channel) const {
  band = current_band;
  channel = current_channel;
}


// RSSI history access methods
#if RSSI_HISTORY_ENABLED
uint32_t TimingCore::getRSSIHistoryCount() const {
  return rssi_history_count;
}

bool TimingCore::getRSSIHistorySample(uint32_t index, RSSISample& sample) const {
  if (!rssi_history_buffer || index >= rssi_history_count) {
    return false;
  }

  uint32_t buffer_index;
  if (rssi_history_count < RSSI_HISTORY_SIZE) {
    buffer_index = index;
  } else {
    buffer_index = (rssi_history_write_index + index) % RSSI_HISTORY_SIZE;
  }

  sample = rssi_history_buffer[buffer_index];
  return true;
}

bool TimingCore::allocateRSSIHistory() {
#if RSSI_HISTORY_ENABLED
  // Don't re-allocate if already allocated
  if (rssi_history_buffer) {
    return true;
  }

  // CALLER MUST NOT HOLD timing_mutex — this can take a while (large
  // malloc + memset). Called from: background task after boot, web race
  // start, LCD start callback.
  size_t required_bytes = RSSI_HISTORY_SIZE * sizeof(RSSISample);

  // Try multiple allocation strategies to handle heap fragmentation
  RSSISample* buf = (RSSISample*)malloc(required_bytes);  // any memory
  if (!buf) {
    buf = (RSSISample*)heap_caps_malloc(required_bytes, MALLOC_CAP_DEFAULT);
  }
  if (!buf) {
    buf = (RSSISample*)heap_caps_malloc(required_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }

  if (!buf) {
    void* test_alloc = malloc(1024);
    if (test_alloc) {
      free(test_alloc);
      Serial.printf("WARNING: RSSI history buffer (%d KB) not allocated - heap fragmented (small allocs work)\n", required_bytes / 1024);
    } else {
      Serial.printf("WARNING: RSSI history buffer (%d KB) not allocated - out of memory\n", required_bytes / 1024);
    }
    return false;
  }

  memset(buf, 0, required_bytes);

  // Publish the finished buffer under the mutex so the timing task never
  // observes a half-initialized buffer.
  if (xSemaphoreTake(timing_mutex, pdMS_TO_TICKS(100))) {
    rssi_history_buffer = buf;
    rssi_history_write_index = 0;
    rssi_history_count = 0;
    last_rssi_sample_time = 0;
    xSemaphoreGive(timing_mutex);
    Serial.printf("RSSI history buffer allocated: %d KB\n", required_bytes / 1024);
    return true;
  }

  // Couldn't publish (timing task holding the lock) — release and let the
  // next call site (race start) retry.
  free(buf);
  return false;
#else
  return false;
#endif
}

void TimingCore::clearRSSIHistory() {
  rssi_history_write_index = 0;
  rssi_history_count = 0;
  last_rssi_sample_time = 0;
  if (rssi_history_buffer) {
    memset(rssi_history_buffer, 0, RSSI_HISTORY_SIZE * sizeof(RSSISample));
  }
}
#endif

