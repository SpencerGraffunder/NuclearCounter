#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <functional>

// Body-streaming HTTP handler for raw-binary POST endpoints.
// It is "trivial" in the framework's sense: form-urlencoded / multipart
// bodies are only counted, never parsed into RAM. The default callback
// handler feeds form bodies to a character parser that accumulates the
// WHOLE body in a String — a 1MB form POST to a raw-body endpoint OOM'd
// the PSRAM-less S3 (board crash-looped until NVS was erased). Clients
// must send Content-Type: application/octet-stream; anything else is
// rejected with 415 after the body (form/multipart bodies are dropped by
// the framework, text/* bodies are rejected on the first byte).
class RawBodyHandler : public AsyncWebHandler {
public:
    using BodyFn = std::function<void(AsyncWebServerRequest*, uint8_t*, size_t, size_t, size_t)>;
    using DoneFn = std::function<void(AsyncWebServerRequest*)>;

    RawBodyHandler(const char* path, BodyFn body, DoneFn done)
        : _path(path), _body(std::move(body)), _done(std::move(done)) {}

    bool canHandle(AsyncWebServerRequest* request) const override {
        return request->method() == HTTP_POST && request->url() == _path;
    }
    void handleBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) override {
        _body(request, data, len, index, total);
    }
    // Runs exactly once per completed POST, after every body byte has been
    // delivered to handleBody (or skipped, for form/multipart bodies).
    void handleRequest(AsyncWebServerRequest* request) override {
        _done(request);
    }
    bool isRequestHandlerTrivial() const override { return true; }

private:
    String _path;
    BodyFn _body;
    DoneFn _done;
};

// Manages WiFi updates of the board:
//
//  1. APP SLOTS (dual-boot)
//     - Only the INACTIVE slot can be updated. The running app can never be
//       overwritten: erasing a slot the CPU is executing from would crash it.
//     - Uploads are streamed in chunks straight into the partition
//       (esp_ota_write) — no large RAM buffers, so it works on the
//       PSRAM-less C3 with its 1.2MB images.
//     - The upload is size-checked up front against the partition size.
//     - otadata is NOT touched until the user explicitly presses "boot".
//       A failed/partial upload leaves an invalid image in the inactive
//       slot, which the board never boots (and the ROM bootloader would
//       fall back to the other slot if it ever were selected).
//
//  2. DATA PARTITION (SPIFFS, the web UI files)
//     - File-level upload / delete on the mounted filesystem.
//     - A whole-image swap is deliberately NOT supported: there is no
//       staging partition in the table and the C3 has no PSRAM to stage in,
//       and raw-writing under a mounted SPIFFS would corrupt it. File-level
//       updates are served immediately (no reboot needed).
class OtaManager {
public:
    enum class OtaState { Idle, Uploading, Ready };

    OtaManager();

    // Register all HTTP routes on the given server.
    void begin(AsyncWebServer* server);

private:

    // ---- app slot handlers ----
    void handleStatus(AsyncWebServerRequest* request);       // GET  /api/ota/status
    void handleStart(AsyncWebServerRequest* request);        // POST /api/ota/start   {"slot":"ota_0","size":N}
    void otaBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total);
    void otaDone(AsyncWebServerRequest* request);            // POST /api/ota/data (raw octet-stream body)
    void handleFinish(AsyncWebServerRequest* request);       // POST /api/ota/finish
    void handleAbort(AsyncWebServerRequest* request);        // POST /api/ota/abort
    void handleBoot(AsyncWebServerRequest* request);         // POST /api/ota/boot    {"slot":"ota_0"}

    // ---- data partition (SPIFFS) handlers ----
    void dataBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total);
    void dataDone(AsyncWebServerRequest* request);           // POST /api/data/write?path=... (raw octet-stream body)
    void handleDataDelete(AsyncWebServerRequest* request);   // POST /api/data/delete {"path":...}

    // Helpers
    const esp_partition_t* findSlot(const String& label);
    bool isValidDataPath(const String& path, String& normalized);

    OtaState state = OtaState::Idle;
    const esp_partition_t* target = nullptr;
    esp_ota_handle_t otaHandle = 0;
    size_t received = 0;
    size_t expected = 0;
    uint32_t lastChunkMs = 0;
    SemaphoreHandle_t otaMutex = nullptr;

    // Per-POST result of the most recent raw-body upload (set by the body
    // callback, consumed exactly once by the done callback). Separate fields
    // per endpoint: two different POSTs may interleave segment-by-segment.
    int otaErrCode = 0;
    String otaErrMsg;
    int dataErrCode = 0;
    String dataErrMsg;

    // Data-partition write state (one file at a time)
    bool dataWriting = false;
    String dataPath;
    File dataFile;
    size_t dataReceived = 0;
    size_t dataExpected = 0;
};

#endif // OTA_MANAGER_H
