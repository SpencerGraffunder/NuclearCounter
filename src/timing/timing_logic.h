// ---------------------------------------------------------------------------
// Pure (hardware-free) timing logic, shared between the firmware and the
// native unit tests ([env:Test], `pio test -e Test`).
//
// Everything in this header must stay free of Arduino/FreeRTOS/ESP includes:
// the same code is compiled on the host by g++/clang in the test build. The
// firmware (timing_core.cpp) is the only translation unit that mixes these
// with hardware — it feeds filtered RSSI samples in and consumes the events.
//
// The behavior here is a LINE-FOR-LINE extraction of the state machine that
// used to live inline in TimingCore::timingTask() (peak capture, min-lap
// gate, crossing events, lap recording) and TimingCore::processExtremums().
// When changing it, keep the host tests in test/unity/ green AND re-verify on
// hardware — the tests pin the exact semantics, including the quirks:
//   * a rejected (too-soon) lap does NOT reset rssi_peak, so the same peak
//     can later be recorded "late" with its original peak timestamp;
//   * the lap ring overwrites the oldest UNCONSUMED lap when it wraps (the
//     >50-laps freeze was fixed upstream by real-time forwarding, but the
//     ring semantics themselves are unchanged).
// ---------------------------------------------------------------------------

#ifndef TIMING_LOGIC_H
#define TIMING_LOGIC_H

#include <cstddef>
#include <cstdint>
#include <cstring>

// Structure to hold lap data (shared with the web API / node protocol)
struct LapData {
  uint32_t timestamp_ms;    // Lap timestamp in milliseconds (PEAK time)
  uint16_t lap_time_ms;     // Time since last lap (0 for first lap)
  uint8_t rssi_peak;        // Peak RSSI during this lap
  uint8_t pilot_id;         // Pilot ID (0-based)
  bool valid;               // Whether this lap data is valid
};

// Structure to hold RSSI extremum data (for marshal mode history)
struct Extremum {
  uint8_t rssi;             // RSSI value at extremum
  uint32_t first_time;      // Timestamp when extremum started (milliseconds)
  uint16_t duration;        // Duration extremum was held (milliseconds)
  bool valid;               // Whether this extremum is valid
};

#define EXTREMUM_BUFFER_SIZE 128  // Circular buffer for extremums (power of 2)

// ---------------------------------------------------------------------------
// Lap ring buffer — the old lap_buffer[]/lap_write_index/lap_read_index.
// push() overwrites whatever slot the write index points at; when the writer
// laps the reader that means the oldest unconsumed lap is dropped.
// ---------------------------------------------------------------------------
template <uint8_t N>
class LapRingBuffer {
public:
  LapRingBuffer() { reset(); }

  void push(const LapData& lap) {
    buf_[write_] = lap;
    write_ = (uint8_t)((write_ + 1) % N);
  }
  bool hasNew() const { return read_ != write_; }
  LapData pop() {
    if (!hasNew()) {
      return LapData{0, 0, 0, 0, false};
    }
    LapData lap = buf_[read_];
    read_ = (uint8_t)((read_ + 1) % N);
    return lap;
  }
  // Slot of the most recently pushed lap (callers check lap count first,
  // mirroring the old TimingCore::getLastLap()).
  LapData last() const {
    return buf_[(write_ - 1 + N) % N];
  }
  uint8_t available() const {
    return (uint8_t)((write_ - read_ + N) % N);
  }
  void reset() {
    std::memset(buf_, 0, sizeof(buf_));
    write_ = 0;
    read_ = 0;
  }
  static constexpr uint8_t capacity() { return N; }

private:
  LapData buf_[N];
  uint8_t write_ = 0;
  uint8_t read_ = 0;
};

// ---------------------------------------------------------------------------
// LapDetector — the per-sample lap-detection state machine.
//
// Call process(now_ms, filtered_rssi) once per sample (1 ms cadence in the
// firmware). It tracks the nadir, applies the configurable min-lap capture
// gate, captures the RSSI peak, emits crossing start/stop events, and
// records a lap when the peak has been captured (RSSI dropped below BOTH the
// captured peak and the exit threshold) and at least `hard_min_lap_ms` has
// elapsed since the last recorded lap (anti-bounce).
//
// NOTE (quirk, pinned by test): when a lap is rejected as too soon, the
// captured peak is NOT cleared. A later sample that is below that stale peak
// and below the exit threshold will record the lap "late" — with the
// ORIGINAL peak timestamp. That is the shipping behavior; do not "fix" it
// without re-calibrating on hardware.
// ---------------------------------------------------------------------------
class LapDetector {
public:
  struct Events {
    bool lap_recorded = false;
    bool crossing_started = false;
    bool crossing_ended = false;
    // Peak captured but rejected as too soon after the previous lap.
    bool lap_rejected = false;
    // For lap_recorded/lap_rejected: ms since the previous lap's peak time
    // (UINT32_MAX when there was no previous lap).
    uint32_t since_last_lap_ms = 0;
  };

  typedef void (*LapCallback)(const LapData&);
  typedef void (*CrossingCallback)(bool crossing_state, uint8_t rssi);

  LapDetector() { initDefaults(); }

  void setEnterRssi(uint8_t v) { enter_rssi_ = v; }
  void setExitRssi(uint8_t v) { exit_rssi_ = v; }
  void setMinLapMs(uint32_t v) { min_lap_ms_ = v; }
  void setHardMinLapMs(uint32_t v) { hard_min_lap_ms_ = v; }

  // Feed one filtered RSSI sample.
  Events process(uint32_t now_ms, uint8_t rssi) {
    Events ev;

    // Update nadir tracking (always track, regardless of crossing state)
    if (rssi < nadir_rssi_) {
      nadir_rssi_ = rssi;
    }
    if (rssi < pass_nadir_rssi_) {
      pass_nadir_rssi_ = rssi;
    }

    // Check if minimum lap time has elapsed (prevents peak capture too soon)
    bool can_capture_peak = true;
    if (min_lap_ms_ > 0 && lap_start_ms_ > 0) {
      uint32_t time_since_lap_start = now_ms - lap_start_ms_;
      if (time_since_lap_start < min_lap_ms_) {
        can_capture_peak = false;
      }
    }

    // Capture peak only when RSSI >= enterRssi and min lap time has elapsed
    if (can_capture_peak && rssi >= enter_rssi_) {
      if (rssi > rssi_peak_) {
        rssi_peak_ = rssi;
        rssi_peak_time_ms_ = now_ms;
        peak_rssi_ = rssi;
      }
    }

    // Check if peak is captured (RSSI dropped below peak AND below exit threshold)
    bool peak_captured = (rssi < rssi_peak_) && (rssi < exit_rssi_);

    // Update crossing state based on whether we're capturing a peak
    bool was_crossing = crossing_active_;
    crossing_active_ = (can_capture_peak && rssi >= enter_rssi_);
    if (was_crossing != crossing_active_) {
      if (crossing_active_) {
        crossing_start_ = now_ms;
      }
      ev.crossing_started = crossing_active_;
      ev.crossing_ended = !crossing_active_;
      if (crossing_callback_) {
        crossing_callback_(crossing_active_, rssi);
      }
    }

    // Record a lap when the peak is captured and enough time has passed
    if (peak_captured && rssi_peak_ > 0) {
      uint32_t time_since_last_lap = (last_lap_time_ > 0)
                                          ? (now_ms - last_lap_time_)
                                          : UINT32_MAX;
      if (time_since_last_lap >= hard_min_lap_ms_) {
        recordLap(rssi_peak_time_ms_, rssi_peak_);
        // Reset pass nadir after lap recorded
        pass_nadir_rssi_ = 255;
        ev.lap_recorded = true;
        ev.since_last_lap_ms = time_since_last_lap;
      } else {
        ev.lap_rejected = true;
        ev.since_last_lap_ms = time_since_last_lap;
      }
    }

    return ev;
  }

  // The lap produced by the most recent process() with lap_recorded.
  const LapData& lastLap() const { return last_lap_; }

  void setLapCallback(LapCallback cb) { lap_callback_ = cb; }
  void setCrossingCallback(CrossingCallback cb) { crossing_callback_ = cb; }

  // State accessors (map 1:1 onto TimingState fields)
  uint8_t enterRssi() const { return enter_rssi_; }
  uint8_t exitRssi() const { return exit_rssi_; }
  uint32_t minLapMs() const { return min_lap_ms_; }
  uint8_t peakRssi() const { return peak_rssi_; }
  uint8_t nadirRssi() const { return nadir_rssi_; }
  uint8_t passNadirRssi() const { return pass_nadir_rssi_; }
  bool crossingActive() const { return crossing_active_; }
  uint32_t crossingStart() const { return crossing_start_; }
  uint32_t lastLapTime() const { return last_lap_time_; }
  uint16_t lapCount() const { return lap_count_; }
  uint32_t rssiPeak() const { return rssi_peak_; }
  uint32_t rssiPeakTimeMs() const { return rssi_peak_time_ms_; }
  uint32_t lapStartTimeMs() const { return lap_start_ms_; }
  uint32_t raceStartMs() const { return race_start_ms_; }
  void setRaceStartMs(uint32_t v) { race_start_ms_ = v; }

  // Mirrors the old TimingCore::reset() timing-related section EXACTLY:
  // it clears lap/timing state but deliberately does NOT touch the
  // enter/exit/min-lap configuration (a reset mid-configuration must keep
  // the user's thresholds).
  void reset() {
    lap_count_ = 0;
    last_lap_time_ = 0;
    peak_rssi_ = 0;
    nadir_rssi_ = 255;
    pass_nadir_rssi_ = 255;
    crossing_active_ = false;
    rssi_peak_ = 0;
    rssi_peak_time_ms_ = 0;
    lap_start_ms_ = 0;
    race_start_ms_ = 0;
  }

private:
  void initDefaults() {
    enter_rssi_ = 120;  // ENTER_RSSI
    exit_rssi_ = 100;   // EXIT_RSSI
    min_lap_ms_ = 0;    // MIN_LAP_MS
    hard_min_lap_ms_ = 3000;  // MIN_LAP_TIME_MS
    nadir_rssi_ = 255;
    pass_nadir_rssi_ = 255;
    crossing_active_ = false;
    crossing_start_ = 0;
    last_lap_time_ = 0;
    lap_count_ = 0;
    rssi_peak_ = 0;
    rssi_peak_time_ms_ = 0;
    lap_start_ms_ = 0;
    race_start_ms_ = 0;
    std::memset(&last_lap_, 0, sizeof(last_lap_));
  }

private:
  void recordLap(uint32_t timestamp, uint8_t peak_rssi) {
    // Use peak timestamp for lap timing
    last_lap_.timestamp_ms = timestamp;

    // First lap uses race start time, others use previous lap start time
    if (lap_count_ == 0) {
      last_lap_.lap_time_ms =
          (race_start_ms_ > 0) ? (timestamp - race_start_ms_) : 0;
    } else {
      last_lap_.lap_time_ms =
          (lap_start_ms_ > 0) ? (timestamp - lap_start_ms_) : 0;
    }

    last_lap_.rssi_peak = peak_rssi;
    last_lap_.pilot_id = 0;  // Single pilot for now
    last_lap_.valid = true;

    last_lap_time_ = timestamp;
    lap_count_++;
    lap_start_ms_ = timestamp;  // start of the NEXT lap's window

    // Reset peak tracking for next lap
    rssi_peak_ = 0;
    rssi_peak_time_ms_ = 0;
    peak_rssi_ = 0;

    if (lap_callback_) {
      lap_callback_(last_lap_);
    }
  }

  uint8_t enter_rssi_ = 120;
  uint8_t exit_rssi_ = 100;
  uint32_t min_lap_ms_ = 0;
  uint32_t hard_min_lap_ms_ = 3000;

  uint8_t nadir_rssi_ = 255;
  uint8_t pass_nadir_rssi_ = 255;
  uint8_t peak_rssi_ = 0;  // last captured peak (TimingState.peak_rssi)

  bool crossing_active_ = false;
  uint32_t crossing_start_ = 0;

  uint32_t last_lap_time_ = 0;
  uint16_t lap_count_ = 0;

  // Peak tracking
  uint8_t rssi_peak_ = 0;
  uint32_t rssi_peak_time_ms_ = 0;
  uint32_t lap_start_ms_ = 0;
  uint32_t race_start_ms_ = 0;

  LapData last_lap_;

  LapCallback lap_callback_ = nullptr;
  CrossingCallback crossing_callback_ = nullptr;
};

// ---------------------------------------------------------------------------
// ExtremumTracker — RSSI peak/nadir history for marshal mode.
// Line-for-line extraction of processExtremums()/finalizePeak()/
// finalizeNadir()/bufferPeak()/bufferNadir() and their ring buffers.
// ---------------------------------------------------------------------------
class ExtremumTracker {
public:
  ExtremumTracker() { reset(); }

  void process(uint32_t timestamp, uint8_t filtered_rssi) {
    // Detect RSSI change direction
    const int rssi_change = (int)filtered_rssi - (int)last_rssi_;

    if (rssi_change > 0) {
      // RSSI is rising
      // Must buffer latest peak to prevent losing it (overwriting any unsent peak)
      bufferPeak(current_peak_);

      // Start tracking new peak with current RSSI value
      current_peak_.rssi = filtered_rssi;
      current_peak_.first_time = timestamp;
      current_peak_.duration = 0;
      current_peak_.valid = true;

      // If RSSI was falling or unchanged, but it's rising now, we found a nadir
      if (rssi_change_ <= 0 && current_nadir_.valid) {
        finalizeNadir(timestamp);
      }

    } else if (rssi_change < 0) {
      // RSSI is falling
      // Must buffer latest nadir to prevent losing it (overwriting any unsent nadir)
      bufferNadir(current_nadir_);

      // Start tracking new nadir with current RSSI value
      current_nadir_.rssi = filtered_rssi;
      current_nadir_.first_time = timestamp;
      current_nadir_.duration = 0;
      current_nadir_.valid = true;

      // If RSSI was rising or unchanged, but it's falling now, we found a peak
      if (rssi_change_ >= 0 && current_peak_.valid) {
        finalizePeak(timestamp);
      }

    } else {
      // RSSI is unchanged
      if (filtered_rssi == current_peak_.rssi && current_peak_.valid) {
        // Still at peak - extend duration
        uint32_t duration = timestamp - current_peak_.first_time;
        current_peak_.duration = (duration > 0xFFFF) ? 0xFFFF : (uint16_t)duration;

        // If peak duration maxed out, buffer it and start a new one
        if (current_peak_.duration == 0xFFFF) {
          bufferPeak(current_peak_);
          current_peak_.rssi = filtered_rssi;
          current_peak_.first_time = timestamp;
          current_peak_.duration = 0;
          current_peak_.valid = true;
        }
      } else if (filtered_rssi == current_nadir_.rssi && current_nadir_.valid) {
        // Still at nadir - extend duration
        uint32_t duration = timestamp - current_nadir_.first_time;
        current_nadir_.duration = (duration > 0xFFFF) ? 0xFFFF : (uint16_t)duration;

        if (current_nadir_.duration == 0xFFFF) {
          bufferNadir(current_nadir_);
          current_nadir_.rssi = filtered_rssi;
          current_nadir_.first_time = timestamp;
          current_nadir_.duration = 0;
          current_nadir_.valid = true;
        }
      }
    }

    // Update state for next iteration
    last_rssi_ = filtered_rssi;

    // Clamp rssiChange to prevent overflow (like RotorHazard does)
    if (rssi_change > 127) {
      rssi_change_ = 127;
    } else if (rssi_change < -127) {
      rssi_change_ = -127;
    } else {
      rssi_change_ = (int8_t)rssi_change;
    }
  }

  bool hasPendingPeak() const { return peak_read_ != peak_write_; }
  bool hasPendingNadir() const { return nadir_read_ != nadir_write_; }

  Extremum getNextPeak() {
    if (!hasPendingPeak()) {
      return Extremum{0, 0, 0, false};
    }
    Extremum peak = peak_buffer_[peak_read_];
    peak_read_ = (uint8_t)((peak_read_ + 1) & (EXTREMUM_BUFFER_SIZE - 1));
    return peak;
  }
  Extremum getNextNadir() {
    if (!hasPendingNadir()) {
      return Extremum{255, 0, 0, false};
    }
    Extremum nadir = nadir_buffer_[nadir_read_];
    nadir_read_ = (uint8_t)((nadir_read_ + 1) & (EXTREMUM_BUFFER_SIZE - 1));
    return nadir;
  }
  Extremum peekNextPeak() const {
    if (peak_read_ == peak_write_) {
      return Extremum{0, 0, 0, false};
    }
    return peak_buffer_[peak_read_];
  }
  Extremum peekNextNadir() const {
    if (nadir_read_ == nadir_write_) {
      return Extremum{255, 0, 0, false};
    }
    return nadir_buffer_[nadir_read_];
  }

  uint8_t lastRssi() const { return last_rssi_; }
  int8_t rssiChange() const { return rssi_change_; }

  void reset() {
    std::memset(peak_buffer_, 0, sizeof(peak_buffer_));
    std::memset(nadir_buffer_, 0, sizeof(nadir_buffer_));
    peak_write_ = 0;
    peak_read_ = 0;
    nadir_write_ = 0;
    nadir_read_ = 0;
    last_rssi_ = 0;
    rssi_change_ = 0;
    std::memset(&current_peak_, 0, sizeof(current_peak_));
    std::memset(&current_nadir_, 0, sizeof(current_nadir_));
    current_nadir_.rssi = 255;
  }

private:
  void finalizePeak(uint32_t timestamp) {
    if (current_peak_.valid && current_peak_.rssi > 0) {
      uint32_t duration = timestamp - current_peak_.first_time;
      current_peak_.duration = (duration > 0xFFFF) ? 0xFFFF : (uint16_t)duration;
      bufferPeak(current_peak_);
    }
    std::memset(&current_peak_, 0, sizeof(current_peak_));
  }
  void finalizeNadir(uint32_t timestamp) {
    if (current_nadir_.valid && current_nadir_.rssi < 255) {
      uint32_t duration = timestamp - current_nadir_.first_time;
      current_nadir_.duration = (duration > 0xFFFF) ? 0xFFFF : (uint16_t)duration;
      bufferNadir(current_nadir_);
    }
    std::memset(&current_nadir_, 0, sizeof(current_nadir_));
    current_nadir_.rssi = 255;
  }
  void bufferPeak(const Extremum& peak) {
    // Only buffer valid peaks (RotorHazard checks validity before buffering)
    if (!peak.valid || peak.rssi == 0) {
      return;
    }
    peak_buffer_[peak_write_] = peak;
    peak_write_ = (uint8_t)((peak_write_ + 1) & (EXTREMUM_BUFFER_SIZE - 1));
    // Check for overflow (if write catches read, drop oldest)
    if (peak_write_ == peak_read_) {
      peak_read_ = (uint8_t)((peak_read_ + 1) & (EXTREMUM_BUFFER_SIZE - 1));
    }
  }
  void bufferNadir(const Extremum& nadir) {
    // Only buffer valid nadirs (RotorHazard checks validity before buffering)
    if (!nadir.valid || nadir.rssi == 255) {
      return;
    }
    nadir_buffer_[nadir_write_] = nadir;
    nadir_write_ = (uint8_t)((nadir_write_ + 1) & (EXTREMUM_BUFFER_SIZE - 1));
    if (nadir_write_ == nadir_read_) {
      nadir_read_ = (uint8_t)((nadir_read_ + 1) & (EXTREMUM_BUFFER_SIZE - 1));
    }
  }

  uint8_t last_rssi_ = 0;
  int8_t rssi_change_ = 0;

  Extremum peak_buffer_[EXTREMUM_BUFFER_SIZE];
  uint8_t peak_write_ = 0;
  uint8_t peak_read_ = 0;

  Extremum nadir_buffer_[EXTREMUM_BUFFER_SIZE];
  uint8_t nadir_write_ = 0;
  uint8_t nadir_read_ = 0;

  Extremum current_peak_;
  Extremum current_nadir_;
};

// ---------------------------------------------------------------------------
// LapStats — pure race statistics over a lap array (OLED best / best-3).
// Line-for-line extraction of IntegratedMode::bestLapMs() and
// best3ConsecutiveMs().
// ---------------------------------------------------------------------------
namespace LapStats {

inline uint32_t bestLapMs(const LapData* laps, size_t n) {
  uint32_t best = 0;
  for (size_t i = 0; i < n; i++) {
    if (laps[i].valid && laps[i].lap_time_ms > 0 &&
        (best == 0 || laps[i].lap_time_ms < best)) {
      best = laps[i].lap_time_ms;
    }
  }
  return best;
}

// Fastest sum of any 3 consecutive laps (the "best 3 in a row" racing metric).
inline uint32_t best3ConsecutiveMs(const LapData* laps, size_t n) {
  if (n < 3) {
    return 0;
  }
  uint32_t best = 0;
  for (size_t i = 0; i + 2 < n; i++) {
    if (!(laps[i].valid && laps[i + 1].valid && laps[i + 2].valid)) {
      continue;
    }
    uint32_t sum = laps[i].lap_time_ms + laps[i + 1].lap_time_ms +
                   laps[i + 2].lap_time_ms;
    if (sum > 0 && (best == 0 || sum < best)) {
      best = sum;
    }
  }
  return best;
}

}  // namespace LapStats

#endif  // TIMING_LOGIC_H
