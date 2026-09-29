#ifndef OTA_H
#define OTA_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <functional>

// Raw-body upload handler. Registered via addHandler() instead of
// on(..., onBody): making isRequestHandlerTrivial() true means form-encoded
// and multipart bodies are only counted, never parsed into RAM (the framework
// would otherwise buffer the entire body as form parameters and OOM the
// board). handleBody() still streams application/octet-stream bytes
// per-chunk. The done callback runs exactly once per completed POST
// (_runChain at body end) and is the single place the HTTP response is sent.
class RawBodyHandler : public AsyncWebHandler {
public:
  using BodyFn = std::function<void(AsyncWebServerRequest *request, uint8_t *data,
                                   size_t len, size_t index, size_t total)>;
  using DoneFn = std::function<void(AsyncWebServerRequest *request)>;
  RawBodyHandler(const String &uri, BodyFn onBody, DoneFn onDone)
      : _uri(uri), _onBody(onBody), _onDone(onDone) {}
  bool canHandle(AsyncWebServerRequest *request) const override {
    return request && request->method() == HTTP_POST && request->url() == _uri;
  }
  bool isRequestHandlerTrivial() const override { return true; }
  void handleRequest(AsyncWebServerRequest *request) override {
    if (_onDone) _onDone(request);
  }
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override {
    if (_onBody) _onBody(request, data, len, index, total);
  }
  void handleUpload(AsyncWebServerRequest *request, const String &filename, size_t index,
                    uint8_t *data, size_t len, bool final) override {}
private:
  String _uri;
  BodyFn _onBody;
  DoneFn _onDone;
};

// WiFi update of the OTHER app slot (StarForgeOS) while NuclearCounter runs.
// Same safety model as the SFOS-side OtaManager:
//  - only the inactive slot can be written (running app is protected)
//  - streamed chunk-by-chunk into the partition (no big RAM buffers)
//  - size-checked up front; image validated by esp_ota_end()
//  - boot switch only on explicit request
//  - form/multipart bodies are never parsed (RawBodyHandler above)
class OtaUpdater {
public:
  OtaUpdater();
  void begin(AsyncWebServer *server);

private:
  enum class State { Idle, Uploading, Ready };
  static const char *stateName(State s);

  void handleRoot(AsyncWebServerRequest *request);
  void handleStatus(AsyncWebServerRequest *request);
  void handleStart(AsyncWebServerRequest *request);
  void otaBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
  void otaDone(AsyncWebServerRequest *request);            // POST /api/ota/data (raw octet-stream body)
  void handleFinish(AsyncWebServerRequest *request);
  void handleAbort(AsyncWebServerRequest *request);
  void handleBoot(AsyncWebServerRequest *request);

  State state = State::Idle;
  const esp_partition_t *target = nullptr;
  esp_ota_handle_t handle = 0;
  size_t received = 0;
  size_t expected = 0;
  uint32_t lastChunkMs = 0;
  int errCode = 0;   // per-POST error set by otaBody, consumed by otaDone
  String errMsg;
  SemaphoreHandle_t mutex = nullptr;
};

#endif
