#include "web_server.h"
#include "config/config.h"
#include "about.h"  // VERSION / BUILD_SHA for the release check (see app.js)
#include "settings/settings_manager.h"
#include "generated/ui_pack.h"  // web UI embedded in the image (tools/build_ui_pack.py)
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Buffer sizes for JSON responses (with safety margins)
#define JSON_STATUS_BUFFER_SIZE 640      // Status response (most frequent)
#define JSON_LAPS_BUFFER_SIZE 16384       // Laps response (100 laps max: ~160 bytes each)
#define JSON_RSSI_BUFFER_SIZE 32768       // RSSI history (30K samples max: ~20 bytes each)
#define JSON_CHANNELS_BUFFER_SIZE 2048    // Channels response (static data)

// Helper structure for delayed timing resume (non-blocking)
struct TimingResumeData {
    TimingCore* timingCore;
    bool wasActive;
    uint32_t delayMs;
};

// FreeRTOS task to resume timing after delay (non-blocking, prevents watchdog timeout)
void resumeTimingAfterDelay(void* parameter) {
    TimingResumeData* data = static_cast<TimingResumeData*>(parameter);
    if (data && data->timingCore && data->wasActive) {
        // Wait for the calculated delay
        vTaskDelay(pdMS_TO_TICKS(data->delayMs));
        // Resume timing
        data->timingCore->resumeFromPause(data->wasActive);
    }
    // Free the allocated structure
    delete data;
    // Delete this one-shot task
    vTaskDelete(NULL);
}

// Helper function to schedule delayed timing resume (non-blocking)
static void scheduleTimingResume(TimingCore* timingCore, bool wasActive, uint32_t delayMs) {
    if (!timingCore || !wasActive) return;  // Nothing to resume
    
    TimingResumeData* data = new TimingResumeData;
    data->timingCore = timingCore;
    data->wasActive = wasActive;
    data->delayMs = delayMs;
    
    // Create a one-shot task that will resume timing after delay
    // Low priority (1) so it doesn't interfere with TCP/IP stack
    xTaskCreate(
        resumeTimingAfterDelay,
        "TimingResume",
        2048,  // Stack size
        data,
        1,      // Low priority (below timing core)
        NULL    // Don't need handle
    );
}

// Helper function to find band/channel from frequency (same lookup as timing_core)
// This ensures band/channel are updated when frequency is set, so they persist correctly
static void findBandChannelFromFrequency(uint16_t freq, uint8_t& band, uint8_t& channel) {
    // Frequency table matching timing_core.cpp
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

    uint16_t min_diff = 65535;
    band = 0;
    channel = 0;

    // Find closest match in frequency table
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

WebServerManager::WebServerManager() : _server(80), _timingCore(nullptr),
    _settingsManager(nullptr), _raceActive(nullptr), 
    _raceStartTime(nullptr), _laps(nullptr)
#if ENABLE_BATTERY_MONITOR && defined(BATTERY_ADC_PIN)
    , _cachedBatteryVoltage(0.0f)
    , _cachedBatteryPercentage(0)
    , _cachedBatteryCharging(false)
    , _batteryDataValid(false)
#endif
{
    // Constructor
}

void WebServerManager::begin(TimingCore* timingCore, SettingsManager* settingsManager,
                              bool* raceActive, uint32_t* raceStartTime, std::vector<LapData>* laps) {
    _timingCore = timingCore;
    _settingsManager = settingsManager;
    _raceActive = raceActive;
    _raceStartTime = raceStartTime;
    _laps = laps;

    // One-time init only: route registration. The IP wait, mDNS
    // and the TCP listener itself live in start(), which runs on every AP
    // bring-up (the AP is torn down on timer-page exit, so the listener
    // must be re-bound each time).
    // Route registration is pure in-memory handler-list work — it does not
    // need the TCP/IP stack up; only _server.begin() (in start()) does.

    // The web UI is compiled into this image (generated/ui_pack.h) — there is
    // no filesystem to mount and nothing for a flash step to forget. The old
    // SPIFFS-partition UI could silently stay stale after an OTA because the
    // OTA only wrote the app slot; embedding the UI makes app+UI one atomic
    // update, protected by the rollback slot together.
    size_t uiTotal = 0;
    for (unsigned i = 0; i < uipack::kCount; i++) uiTotal += uipack::kEntries[i].size;
    VPRINT("Web UI embedded in image: %u files, %u bytes\n", (unsigned)uipack::kCount, (unsigned)uiTotal);

    // No delay here on purpose. Route registration is pure in-memory handler-list
    // work; only _server.begin() (in start()) needs the TCP/IP stack. This used to
    // carry a delay(500) "wait for TCP/IP" that ran once, on the first timer entry,
    // for no reason.
    VPRINT("Registering web server routes...\n");
    
    // Test route to verify server is working
    _server.on("/test", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "text/plain", "Web server is working!");
    });
    
    _server.on("/", HTTP_GET, [this](AsyncWebServerRequest* request) { handleRoot(request); });
    _server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* request) { handleGetStatus(request); });
    _server.on("/api/laps", HTTP_GET, [this](AsyncWebServerRequest* request) { handleGetLaps(request); });
    _server.on("/api/rssi_history", HTTP_GET, [this](AsyncWebServerRequest* request) { handleGetRSSIHistory(request); });
    _server.on("/api/start_race", HTTP_POST, [this](AsyncWebServerRequest* request) { handleStartRace(request); });
    _server.on("/api/stop_race", HTTP_POST, [this](AsyncWebServerRequest* request) { handleStopRace(request); });
    _server.on("/api/clear_laps", HTTP_POST, [this](AsyncWebServerRequest* request) { handleClearLaps(request); });
    _server.on("/api/set_frequency", HTTP_POST, [this](AsyncWebServerRequest* request) { handleSetFrequency(request); });
    _server.on("/api/set_threshold", HTTP_POST, [this](AsyncWebServerRequest* request) { handleSetThreshold(request); });
    _server.on("/api/get_channels", HTTP_GET, [this](AsyncWebServerRequest* request) { handleGetChannels(request); });
#ifdef TIMING_JITTER_PROBE
    // Bench-only (compiled out of every production env; see [env:JITTER_C3]).
    // Sample-interval stats are served over HTTP on purpose: opening this
    // board's USB serial port resets it (rst:0x15 USB_UART_CHIP_RESET), which
    // would drop the AP mid-measurement.
    _server.on("/api/jitter", HTTP_GET, [this](AsyncWebServerRequest* request) { handleJitter(request); });
#endif
#ifdef TIMER_TRACE
    // Bench-only: read back the TTRACE() stage records (see src/config/config.h).
    // Served over HTTP because printing them would perturb the very timing being
    // measured (a USB-CDC write to an undrained port blocks).
    _server.on("/api/timerstats", HTTP_GET, [this](AsyncWebServerRequest* request) { handleTimerTrace(request); });
#endif

    // OTA: app-slot firmware updates (the web UI is embedded in the image,
    // so a single OTA updates both)
    _otaManager.begin(&_server);
    _server.on("/style.css", HTTP_GET, [this](AsyncWebServerRequest* request) { handleStyleCSS(request); });
    _server.on("/app.js", HTTP_GET, [this](AsyncWebServerRequest* request) { handleAppJS(request); });
    _server.onNotFound([this](AsyncWebServerRequest* request) { handleNotFound(request); });

    VPRINT("Web server routes registered (listener starts in start())\n");
}

// Per-AP-session bring-up: called on every timer-page entry, after the AP
// is up.
void WebServerManager::start() {
    TTRACE("web.start: begin");

    // Wait for the AP IP to be assigned — that is what proves the TCP/IP stack
    // is up, which is all _server.begin() needs. softAP() has already returned a
    // valid IP in practice, so this loop normally runs zero iterations; the 3 s
    // cap is a safety net, not an expected cost. It used to cap at 10 s and print
    // its way through the wait — that is where a "10 second" timer entry came
    // from whenever the AP did not come up cleanly.
    int wifiWaitCount = 0;
    const int maxWait = 30;  // 3 s
    while (WiFi.softAPIP().toString() == "0.0.0.0" && wifiWaitCount < maxWait) {
        delay(100);
        wifiWaitCount++;
    }
    if (WiFi.softAPIP().toString() == "0.0.0.0") {
        Serial.println(F("Web: AP IP not assigned, listener not started"));
        return;
    }
    delay(50);  // let the AP interface settle before binding the listener
    TTRACE("web.start: AP IP wait done");

    // Initialize mDNS for .local hostname (after WiFi is ready). Re-called
    // on every AP bring-up: MDNS.begin() returns false when the responder
    // already exists (service already registered), so re-calling is safe.
    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", WEB_SERVER_PORT);
    }
    TTRACE("web.start: mDNS done");

    _server.begin();
    TTRACE("web.start: _server.begin returned");

    // One short line, not six — see VPRINT in src/config/config.h for why the
    // bring-up chatter is compiled out of production builds.
    Serial.printf("Timer web: %s\n", WiFi.softAPIP().toString().c_str());
    VPRINT("mDNS hostname: %s.local\n", MDNS_HOSTNAME);
    VPRINT("Server listening on port %d\n", WEB_SERVER_PORT);
    VPRINT("Open browser to http://192.168.8.1 or http://%s.local\n", MDNS_HOSTNAME);
}

// Close the TCP listener. Routes survive — start() re-binds.
void WebServerManager::stop() {
    TTRACE("web.stop: begin");
    _server.end();
    TTRACE("web.stop: _server.end returned");
    VPRINT("Web server stopped (TCP listener closed)\n");
}


// ===== HTTP Handlers =====

// FNV-1a 32-bit over an embedded UI file, used as a content-derived ETag:
// clients revalidate with If-None-Match and get a 304 (headers only) when the
// firmware they loaded from matches the one their copy came from.
static uint32_t uiFileHash(const uipack::Entry* e) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < e->size; i++) {
        h ^= e->data[i];
        h *= 16777619u;
    }
    return h;
}

static const uipack::Entry* findUiFile(const char* name) {
    for (unsigned i = 0; i < uipack::kCount; i++) {
        if (strcmp(uipack::kEntries[i].name, name) == 0) return &uipack::kEntries[i];
    }
    return nullptr;
}

// Answer a revalidation request with 304 when the client's copy is current.
// Must run BEFORE any timing-core pause: a 304 transfers nothing, so pausing
// the lap detector to save a few KB would be pure loss. Returns true if the
// request was fully answered.
static bool uiRevalidate(AsyncWebServerRequest* request, const uipack::Entry* e) {
    char etag[24];
    snprintf(etag, sizeof(etag), "\"%s-%08lX\"", e->name, (unsigned long)uiFileHash(e));
    if (request->hasHeader("If-None-Match") &&
        request->header("If-None-Match").indexOf(String(etag)) >= 0) {
        AsyncWebServerResponse* notModified = request->beginResponse(304);
        notModified->addHeader("ETag", etag);
        notModified->addHeader("Cache-Control", "no-cache");
        request->send(notModified);
        return true;
    }
    return false;
}

// "no-cache" + content ETag + 304 (see uiRevalidate) — NOT the old
// 'max-age=31536000, immutable' + static ?v= buster. That combination cached
// style.css/app.js for a YEAR with no revalidation, and the ?v= number never
// changed when the files did, so phones kept serving the old UI long after
// re-flashes. (Now the UI is embedded in the image, so a firmware update
// always brings a matching UI — the ETag just saves the re-upload.)
static AsyncWebServerResponse* uiResponse(AsyncWebServerRequest* request, const uipack::Entry* e) {
    char etag[24];
    snprintf(etag, sizeof(etag), "\"%s-%08lX\"", e->name, (unsigned long)uiFileHash(e));
    AsyncWebServerResponse* response = request->beginResponse(
        e->mime, e->size, [e](uint8_t* buffer, size_t len, size_t index) -> size_t {
            if (index >= e->size) return 0;
            if (index + len > e->size) len = e->size - index;
            memcpy(buffer, e->data + index, len);
            return len;
        });
    response->addHeader("Cache-Control", "no-cache");
    response->addHeader("ETag", etag);
    return response;
}

// Full-body serve of an embedded UI file, with the timing core paused for a
// conservative window while the transfer is in flight (same policy the old
// SPIFFS streaming path used; 304 revalidations skip the pause entirely).
// baseMs/capMs are the old per-file values (css: 4s/15s, js: 6s/20s).
static void serveUiFile(AsyncWebServerRequest* request, const uipack::Entry* e,
                        TimingCore* timingCore, uint32_t baseMs, uint32_t capMs) {
    bool timingWasActive = false;
    uint32_t pauseMs = ((e->size * 4000) / 30000) + baseMs;  // ~30KB/s estimate, 4x margin
    if (pauseMs > capMs) pauseMs = capMs;
    if (timingCore) {
        timingWasActive = timingCore->pauseTemporarily(pauseMs);
    }
    request->send(uiResponse(request, e));
    // Resume in a background task (not a blocking delay — watchdog safety).
    scheduleTimingResume(timingCore, timingWasActive, pauseMs);
}

void WebServerManager::handleRoot(AsyncWebServerRequest* request) {
    const uipack::Entry* e = findUiFile("index.html");
    if (!e) {
        request->send(404, "text/plain", "index.html not built into this image");
        return;
    }
    if (uiRevalidate(request, e)) return;
    // No timing pause for index.html (small, and the old path never paused it).
    request->send(uiResponse(request, e));
}

void WebServerManager::handleGetStatus(AsyncWebServerRequest* request) {
    // Pre-allocated buffer for status JSON (most frequently called handler)
    static char jsonBuffer[JSON_STATUS_BUFFER_SIZE];
    
    uint8_t current_rssi = _timingCore ? _timingCore->getCurrentRSSI() : 0;
    uint16_t frequency = _timingCore ? _timingCore->getState().frequency_mhz : 5800;
    uint8_t enter_rssi = _timingCore ? _timingCore->getEnterRssi() : 120;
    uint8_t exit_rssi = _timingCore ? _timingCore->getExitRssi() : 100;
    // Min lap time in whole seconds (0 = disabled); the engine stores ms.
    uint32_t min_lap = _timingCore ? (_timingCore->getMinLapMs() / 1000) : 0;
    bool crossing = _timingCore ? _timingCore->isCrossing() : false;
    size_t lapCount = _laps ? _laps->size() : 0;
    uint32_t uptime = millis();

    // Build JSON using snprintf (bounds-checked, efficient)
    int len = snprintf(jsonBuffer, JSON_STATUS_BUFFER_SIZE,
        "{\"status\":\"%s\",\"lap_count\":%zu,\"uptime\":%lu,\"rssi\":%u,\"frequency\":%u,"
        "\"enter_rssi\":%u,\"exit_rssi\":%u,\"threshold\":%u,\"min_lap\":%lu,\"crossing\":%s,"
        "\"version\":\"%s\",\"build_sha\":\"%s\"",
        (*_raceActive ? "racing" : "ready"), lapCount, uptime, current_rssi, frequency,
        enter_rssi, exit_rssi, enter_rssi, (unsigned long)min_lap, crossing ? "true" : "false",
        VERSION, BUILD_SHA);

#if ENABLE_BATTERY_MONITOR && defined(BATTERY_ADC_PIN)
    // Add battery status if available
    if (_batteryDataValid && len > 0 && len < (int)(JSON_STATUS_BUFFER_SIZE - 100)) {
#if defined(USB_DETECT_PIN)
        len += snprintf(jsonBuffer + len, JSON_STATUS_BUFFER_SIZE - len,
            ",\"battery\":{\"voltage\":%.2f,\"percentage\":%u,\"charging\":%s}",
            _cachedBatteryVoltage, _cachedBatteryPercentage, _cachedBatteryCharging ? "true" : "false");
#else
        len += snprintf(jsonBuffer + len, JSON_STATUS_BUFFER_SIZE - len,
            ",\"battery\":{\"voltage\":%.2f,\"percentage\":%u}",
            _cachedBatteryVoltage, _cachedBatteryPercentage);
#endif
    }
#endif

    // Close JSON and verify buffer didn't overflow
    if (len > 0 && len < (int)JSON_STATUS_BUFFER_SIZE - 2) {
        jsonBuffer[len++] = '}';
        jsonBuffer[len] = '\0';
        request->send(200, "application/json", jsonBuffer);
    } else {
        // Buffer overflow protection
        request->send(500, "application/json", "{\"error\":\"response_too_large\"}");
    }
}

void WebServerManager::handleGetLaps(AsyncWebServerRequest* request) {
    if (!_laps || !_raceStartTime) {
        request->send(500, "application/json", "{\"error\":\"invalid_state\"}");
        return;
    }

    // Pre-allocated buffer for laps JSON (MAX_LAPS_STORED = 100)
    static char jsonBuffer[JSON_LAPS_BUFFER_SIZE];
    int len = 0;
    
    // Limit to MAX_LAPS_STORED to prevent buffer overflow
    size_t lapCount = _laps->size();
    if (lapCount > MAX_LAPS_STORED) {
        lapCount = MAX_LAPS_STORED;
    }

    len = snprintf(jsonBuffer, JSON_LAPS_BUFFER_SIZE, "[");
    if (len < 0 || len >= (int)JSON_LAPS_BUFFER_SIZE) {
        request->send(500, "application/json", "{\"error\":\"buffer_overflow\"}");
        return;
    }

    for (size_t i = 0; i < lapCount; i++) {
        if (i > 0) {
            len += snprintf(jsonBuffer + len, JSON_LAPS_BUFFER_SIZE - len, ",");
            if (len < 0 || len >= (int)JSON_LAPS_BUFFER_SIZE) break;
        }

        uint32_t lapTime = 0;
        if (i == 0) {
            lapTime = (*_laps)[i].timestamp_ms - *_raceStartTime;
        } else {
            lapTime = (*_laps)[i].timestamp_ms - (*_laps)[i-1].timestamp_ms;
        }

        int written = snprintf(jsonBuffer + len, JSON_LAPS_BUFFER_SIZE - len,
            "{\"lap_number\":%zu,\"timestamp_ms\":%lu,\"peak_rssi\":%u,\"lap_time_ms\":%lu}",
            i + 1, (*_laps)[i].timestamp_ms, (*_laps)[i].rssi_peak, lapTime);
        
        if (written < 0 || written >= (int)(JSON_LAPS_BUFFER_SIZE - len)) {
            // Buffer overflow - truncate response
            break;
        }
        len += written;
    }

    // Close JSON array
    if (len > 0 && len < (int)JSON_LAPS_BUFFER_SIZE - 2) {
        len += snprintf(jsonBuffer + len, JSON_LAPS_BUFFER_SIZE - len, "]");
        if (len > 0 && len < (int)JSON_LAPS_BUFFER_SIZE) {
            request->send(200, "application/json", jsonBuffer);
            return;
        }
    }

    // Fallback on error
    request->send(500, "application/json", "{\"error\":\"response_too_large\"}");
}

void WebServerManager::handleGetRSSIHistory(AsyncWebServerRequest* request) {
#if RSSI_HISTORY_ENABLED
    if (!_timingCore) {
        request->send(500, "application/json", "{\"error\":\"Timing core not available\"}");
        return;
    }

    uint32_t count = _timingCore->getRSSIHistoryCount();
    
    // Limit response size to prevent buffer overflow (estimate ~20 bytes per sample)
    // Allow up to ~1500 samples to fit in buffer (30KB / 20 bytes)
    const uint32_t MAX_SAMPLES_IN_RESPONSE = (JSON_RSSI_BUFFER_SIZE - 100) / 20;
    if (count > MAX_SAMPLES_IN_RESPONSE) {
        count = MAX_SAMPLES_IN_RESPONSE;
    }

    static char jsonBuffer[JSON_RSSI_BUFFER_SIZE];
    int len = snprintf(jsonBuffer, JSON_RSSI_BUFFER_SIZE, "{\"count\":%lu,\"samples\":[", count);
    
    if (len < 0 || len >= (int)JSON_RSSI_BUFFER_SIZE) {
        request->send(500, "application/json", "{\"error\":\"buffer_overflow\"}");
        return;
    }

    RSSISample sample;
    uint32_t samplesAdded = 0;
    for (uint32_t i = 0; i < count && samplesAdded < MAX_SAMPLES_IN_RESPONSE; i++) {
        if (_timingCore->getRSSIHistorySample(i, sample)) {
            if (samplesAdded > 0) {
                len += snprintf(jsonBuffer + len, JSON_RSSI_BUFFER_SIZE - len, ",");
                if (len < 0 || len >= (int)JSON_RSSI_BUFFER_SIZE) break;
            }
            
            int written = snprintf(jsonBuffer + len, JSON_RSSI_BUFFER_SIZE - len,
                "{\"t\":%lu,\"r\":%u}", sample.timestamp_ms, sample.rssi);
            
            if (written < 0 || written >= (int)(JSON_RSSI_BUFFER_SIZE - len)) {
                break;  // Buffer overflow
            }
            len += written;
            samplesAdded++;
        }
    }

    // Close JSON
    if (len > 0 && len < (int)JSON_RSSI_BUFFER_SIZE - 3) {
        len += snprintf(jsonBuffer + len, JSON_RSSI_BUFFER_SIZE - len, "]}");
        if (len > 0 && len < (int)JSON_RSSI_BUFFER_SIZE) {
            request->send(200, "application/json", jsonBuffer);
            return;
        }
    }

    request->send(500, "application/json", "{\"error\":\"response_too_large\"}");
#else
    request->send(501, "application/json", "{\"error\":\"RSSI history not enabled\"}");
#endif
}

void WebServerManager::handleStartRace(AsyncWebServerRequest* request) {
    // Allocate the RSSI history buffer at race start, OUTSIDE the timing
    // mutex (and not at boot): a 150KB malloc here must never delay startup
    // or stall the timing task's critical section. No-op if already allocated.
    if (_timingCore) {
        _timingCore->allocateRSSIHistory();
    }

    *_raceActive = true;
    *_raceStartTime = millis();
    _laps->clear();

    if (_timingCore) {
        while (_timingCore->hasNewLap()) {
            _timingCore->getNextLap();
        }
    }

    Serial.println("Race started!");
    request->send(200, "application/json", "{\"status\":\"race_started\"}");
}

void WebServerManager::handleStopRace(AsyncWebServerRequest* request) {
    *_raceActive = false;
    Serial.println("Race stopped!");
    request->send(200, "application/json", "{\"status\":\"race_stopped\"}");
}

void WebServerManager::handleClearLaps(AsyncWebServerRequest* request) {
    _laps->clear();
    Serial.println("Laps cleared!");
    request->send(200, "application/json", "{\"status\":\"laps_cleared\"}");
}

void WebServerManager::handleSetFrequency(AsyncWebServerRequest* request) {
    if (request->hasParam("frequency", true)) {
        int freq = request->getParam("frequency", true)->value().toInt();
        if (freq >= 5645 && freq <= 5945) {
            if (_timingCore) {
                uint8_t band, channel;
                findBandChannelFromFrequency(freq, band, channel);
                _timingCore->setRX5808Settings(band, channel);
                if (_settingsManager) {
                    _settingsManager->saveSettings(_timingCore);
                }
                Serial.printf("Frequency set to: %d MHz (Band=%d, Channel=%d, saved)\n",
                             freq, band, channel);
            }
            // Use snprintf for efficient JSON building
            char response[128];
            snprintf(response, sizeof(response), "{\"status\":\"frequency_set\",\"frequency\":%d}", freq);
            request->send(200, "application/json", response);
        } else {
            request->send(400, "application/json", "{\"error\":\"invalid_frequency\"}");
        }
    } else {
        request->send(400, "application/json", "{\"error\":\"missing_frequency\"}");
    }
}

// Optional min_lap (whole seconds, 0-60, 0 = disabled) applied whenever it is
// present, on any branch — the web sends it alongside the RSSI thresholds so
// one save covers the whole settings row (matches the OLED's save-on-commit).
static void applyMinLapIfPresent(AsyncWebServerRequest* request, TimingCore* core) {
    if (request->hasParam("min_lap", true) && core) {
        int min_lap = request->getParam("min_lap", true)->value().toInt();
        if (min_lap >= 0 && min_lap <= 60) {
            core->setMinLapMs((uint32_t)min_lap * 1000);
            Serial.printf("Min lap set to: %d s\n", min_lap);
        }
    }
}

void WebServerManager::handleSetThreshold(AsyncWebServerRequest* request) {
    static char response[128];
    
    if (request->hasParam("enter_rssi", true) && request->hasParam("exit_rssi", true)) {
        int enter_rssi = request->getParam("enter_rssi", true)->value().toInt();
        int exit_rssi = request->getParam("exit_rssi", true)->value().toInt();
        if (enter_rssi >= 0 && enter_rssi <= 255 && exit_rssi >= 0 && exit_rssi <= 255 && enter_rssi > exit_rssi) {
            if (_timingCore) {
                _timingCore->setEnterRssi(enter_rssi);
                _timingCore->setExitRssi(exit_rssi);
                applyMinLapIfPresent(request, _timingCore);
                if (_settingsManager) {
                    _settingsManager->saveSettings(_timingCore);
                }
            }
            snprintf(response, sizeof(response), "{\"status\":\"threshold_set\",\"enter_rssi\":%d,\"exit_rssi\":%d}",
                    enter_rssi, exit_rssi);
            request->send(200, "application/json", response);
            Serial.printf("Thresholds set: Enter=%d, Exit=%d (saved)\n", enter_rssi, exit_rssi);
        } else {
            request->send(400, "application/json", "{\"error\":\"invalid_threshold\"}");
        }
    } else if (request->hasParam("threshold", true)) {
        int threshold = request->getParam("threshold", true)->value().toInt();
        if (threshold >= 0 && threshold <= 255) {
            uint8_t enter = threshold;
            uint8_t exit = (threshold > 20) ? (threshold - 20) : threshold;

            if (_timingCore) {
                _timingCore->setEnterRssi(enter);
                _timingCore->setExitRssi(exit);
                applyMinLapIfPresent(request, _timingCore);
                if (_settingsManager) {
                    _settingsManager->saveSettings(_timingCore);
                }
            }
            snprintf(response, sizeof(response), "{\"status\":\"threshold_set\",\"threshold\":%d}", threshold);
            request->send(200, "application/json", response);
            Serial.printf("Threshold set to: %d (migrated to Enter=%d, Exit=%d, saved)\n", threshold, enter, exit);
        } else {
            request->send(400, "application/json", "{\"error\":\"invalid_threshold\"}");
        }
    } else if (request->hasParam("min_lap", true)) {
        // Min lap can also be set on its own.
        if (_timingCore) {
            applyMinLapIfPresent(request, _timingCore);
            if (_settingsManager) {
                _settingsManager->saveSettings(_timingCore);
            }
            request->send(200, "application/json", "{\"status\":\"min_lap_set\"}");
        } else {
            request->send(500, "application/json", "{\"error\":\"no_timing_core\"}");
        }
    } else {
        request->send(400, "application/json", "{\"error\":\"missing_threshold\"}");
    }
}

void WebServerManager::handleGetChannels(AsyncWebServerRequest* request) {
    // Static data - use pre-allocated buffer
    static const char* channelsJson = 
        "{\"bands\":{"
        "\"Raceband\":["
        "{\"channel\":\"R1\",\"frequency\":5658},{\"channel\":\"R2\",\"frequency\":5695},"
        "{\"channel\":\"R3\",\"frequency\":5732},{\"channel\":\"R4\",\"frequency\":5769},"
        "{\"channel\":\"R5\",\"frequency\":5806},{\"channel\":\"R6\",\"frequency\":5843},"
        "{\"channel\":\"R7\",\"frequency\":5880},{\"channel\":\"R8\",\"frequency\":5917}],"
        "\"Fatshark\":["
        "{\"channel\":\"F1\",\"frequency\":5740},{\"channel\":\"F2\",\"frequency\":5760},"
        "{\"channel\":\"F3\",\"frequency\":5780},{\"channel\":\"F4\",\"frequency\":5800},"
        "{\"channel\":\"F5\",\"frequency\":5820},{\"channel\":\"F6\",\"frequency\":5840},"
        "{\"channel\":\"F7\",\"frequency\":5860},{\"channel\":\"F8\",\"frequency\":5880}],"
        "\"Boscam_A\":["
        "{\"channel\":\"A1\",\"frequency\":5865},{\"channel\":\"A2\",\"frequency\":5845},"
        "{\"channel\":\"A3\",\"frequency\":5825},{\"channel\":\"A4\",\"frequency\":5805},"
        "{\"channel\":\"A5\",\"frequency\":5785},{\"channel\":\"A6\",\"frequency\":5765},"
        "{\"channel\":\"A7\",\"frequency\":5745},{\"channel\":\"A8\",\"frequency\":5725}],"
        "\"Boscam_E\":["
        "{\"channel\":\"E1\",\"frequency\":5705},{\"channel\":\"E2\",\"frequency\":5685},"
        "{\"channel\":\"E3\",\"frequency\":5665},{\"channel\":\"E4\",\"frequency\":5645},"
        "{\"channel\":\"E5\",\"frequency\":5885},{\"channel\":\"E6\",\"frequency\":5905},"
        "{\"channel\":\"E7\",\"frequency\":5925},{\"channel\":\"E8\",\"frequency\":5945}]"
        "}}";
    
    request->send(200, "application/json", channelsJson);
}

void WebServerManager::handleStyleCSS(AsyncWebServerRequest* request) {
    const uipack::Entry* e = findUiFile("style.css");
    if (!e) {
        // Fallback error CSS (should be impossible — the UI is compiled in).
        request->send(200, "text/css",
            "body{font-family:Arial,sans-serif;background:#1a1f35;color:#fff;padding:40px;text-align:center;}"
            "h1{color:#ff7b00;margin-bottom:20px;}"
            ".error{background:#2a0f0f;border:2px solid #ff3838;border-radius:8px;padding:30px;max-width:600px;margin:0 auto;}"
        );
        return;
    }
    if (uiRevalidate(request, e)) return;  // 304: no timing pause (see serveUiFile)
    serveUiFile(request, e, _timingCore, 4000, 15000);  // old css pause values
}

void WebServerManager::handleAppJS(AsyncWebServerRequest* request) {
    const uipack::Entry* e = findUiFile("app.js");
    if (!e) {
        // Fallback error JavaScript (should be impossible — the UI is compiled in).
        request->send(200, "application/javascript",
            "console.error('app.js not built into this firmware image');"
            "document.body.innerHTML='<div class=\"error\"><h1>⚠️ Files Missing</h1>"
            "<p>Web interface files not found on device.</p>"
            "<p>Please re-flash the board.</p></div>';"
        );
        return;
    }
    if (uiRevalidate(request, e)) return;  // 304: no timing pause (see serveUiFile)
    serveUiFile(request, e, _timingCore, 6000, 20000);  // old js pause values
}

#ifdef TIMING_JITTER_PROBE
void WebServerManager::handleJitter(AsyncWebServerRequest* request) {
    TimingCore::JitterStats s;
    if (!_timingCore || !_timingCore->readJitterStats(s)) {
        request->send(500, "application/json", "{\"error\":\"jitter_read_failed\"}");
        return;
    }
    char buf[400];
    snprintf(buf, sizeof(buf),
             "{\"samples\":%lu,\"window_ms\":%lu,\"avg_us\":%lu,\"max_us\":%lu,\"max_at_ms\":%lu,"
             "\"wait_max_us\":%lu,\"wait_sum_us\":%lu,\"work_max_us\":%lu,"
             "\"outside_max_us\":%lu,\"deact_iters\":%lu,"
             "\"buckets\":{\"lt1k\":%lu,\"1k_1.5k\":%lu,\"1.5k_2k\":%lu,\"2k_3k\":%lu,"
             "\"3k_5k\":%lu,\"5k_10k\":%lu,\"10k_20k\":%lu,\"20k_50k\":%lu,\"gt50k\":%lu}}",
             (unsigned long)s.samples, (unsigned long)s.window_ms,
             s.samples ? (unsigned long)(s.sum_us / s.samples) : 0UL,
             (unsigned long)s.max_us,
             (unsigned long)s.max_at_ms,
             (unsigned long)s.wait_max_us, (unsigned long)s.wait_sum_us,
             (unsigned long)s.work_max_us,
             (unsigned long)s.outside_max_us, (unsigned long)s.deact_iters,
             (unsigned long)s.buckets[0], (unsigned long)s.buckets[1], (unsigned long)s.buckets[2],
             (unsigned long)s.buckets[3], (unsigned long)s.buckets[4], (unsigned long)s.buckets[5],
             (unsigned long)s.buckets[6], (unsigned long)s.buckets[7], (unsigned long)s.buckets[8]);
    request->send(200, "application/json", buf);
}
#endif

#ifdef TIMER_TRACE
void WebServerManager::handleTimerTrace(AsyncWebServerRequest* request) {
    // Compact form: one "stage:elapsed_from_previous_stage" pair per record, so a
    // single read shows where the time actually went.
    static char buf[1600];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "{\"count\":%d,\"stages\":[", g_timerTraceN);
    uint32_t prev = 0;
    for (int i = 0; i < g_timerTraceN && n < (int)sizeof(buf) - 96; i++) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s{\"stage\":\"%s\",\"ms\":%lu,\"delta\":%lu}",
                      i ? "," : "", g_timerTrace[i].stage,
                      (unsigned long)g_timerTrace[i].ms,
                      (unsigned long)(g_timerTrace[i].ms - prev));
        prev = g_timerTrace[i].ms;
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]}");
    request->send(200, "application/json", buf);
}
#endif

void WebServerManager::handleNotFound(AsyncWebServerRequest* request) {
    request->send(404, "text/plain", "File not found");
}

#if ENABLE_BATTERY_MONITOR && defined(BATTERY_ADC_PIN)
void WebServerManager::updateBatteryStatus(float voltage, uint8_t percentage, bool isCharging) {
    // Cache battery data for serving via web API (called from StandaloneMode polling loop)
    _cachedBatteryVoltage = voltage;
    _cachedBatteryPercentage = percentage;
    _cachedBatteryCharging = isCharging;
    _batteryDataValid = true;
}
#endif
