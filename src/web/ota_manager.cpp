#include "ota_manager.h"

// Idle timeout for an in-progress upload (no chunk for this long -> abort).
// Generous: a 1.2MB image over a weak AP link should still finish.
#define OTA_CHUNK_TIMEOUT_MS 120000UL

// Max size of a single data-partition (SPIFFS) file upload.
// The web UI files are tens of KB; 1MB is plenty and bounds flash wear.
#define DATA_FILE_MAX_SIZE (1024UL * 1024UL)

OtaManager::OtaManager() {
    otaMutex = xSemaphoreCreateMutex();
}

static const char* otaStateName(OtaManager::OtaState s) {
    switch (s) {
        case OtaManager::OtaState::Idle: return "idle";
        case OtaManager::OtaState::Uploading: return "uploading";
        case OtaManager::OtaState::Ready: return "ready";
    }
    return "idle";
}

const esp_partition_t* OtaManager::findSlot(const String& label) {
    // Whitelist: only the two app slots may be addressed by name.
    // NB: use the exact OTA subtypes — in this IDF, subtype 0 is NOT "any"
    // (ESP_PARTITION_SUBTYPE_ANY is 0xFF), so a 0 cast silently finds nothing.
    if (label == "ota_0")
        return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0");
    if (label == "ota_1")
        return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, "ota_1");
    return nullptr;
}

void OtaManager::begin(AsyncWebServer* server) {
    server->on("/api/ota/status", HTTP_GET, [this](AsyncWebServerRequest* r) { handleStatus(r); });
    server->on("/api/ota/start", HTTP_POST, [this](AsyncWebServerRequest* r) { handleStart(r); });
    server->addHandler(new RawBodyHandler("/api/ota/data",
        [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t) { otaBody(r, d, l, i, t); },
        [this](AsyncWebServerRequest* r) { otaDone(r); }));
    server->on("/api/ota/finish", HTTP_POST, [this](AsyncWebServerRequest* r) { handleFinish(r); });
    server->on("/api/ota/abort", HTTP_POST, [this](AsyncWebServerRequest* r) { handleAbort(r); });
    server->on("/api/ota/boot", HTTP_POST, [this](AsyncWebServerRequest* r) { handleBoot(r); });

    server->addHandler(new RawBodyHandler("/api/data/write",
        [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t) { dataBody(r, d, l, i, t); },
        [this](AsyncWebServerRequest* r) { dataDone(r); }));
    server->on("/api/data/delete", HTTP_POST, [this](AsyncWebServerRequest* r) { handleDataDelete(r); });
}

// ===== App slot (dual-boot) OTA =====

void OtaManager::handleStatus(AsyncWebServerRequest* request) {
    const esp_partition_t* running = esp_ota_get_running_partition();

    char buf[768];
    int len = snprintf(buf, sizeof(buf),
                       "{\"running\":\"%s\",\"upload\":{\"state\":\"%s\",\"received\":%u,\"expected\":%u},\"slots\":[",
                       running ? running->label : "?",
                       otaStateName(state), (unsigned)received, (unsigned)expected);

    for (const char* label : {"ota_0", "ota_1"}) {
        const esp_partition_t* p = findSlot(label);
        esp_ota_img_states_t imgState = ESP_OTA_IMG_UNDEFINED;
        if (p) esp_ota_get_state_partition(p, &imgState);
        const char* s = imgState == ESP_OTA_IMG_NEW ? "new"
                         : imgState == ESP_OTA_IMG_PENDING_VERIFY ? "pending_verify"
                         : imgState == ESP_OTA_IMG_VALID ? "valid"
                         : imgState == ESP_OTA_IMG_INVALID ? "invalid"
                         : imgState == ESP_OTA_IMG_ABORTED ? "aborted" : "undefined";
        if (len > 0) len += snprintf(buf + len, sizeof(buf) - len, ",");
        len += snprintf(buf + len, sizeof(buf) - len,
                        "{\"label\":\"%s\",\"size\":%u,\"image\":\"%s\",\"running\":%s}",
                        label, p ? (unsigned)p->size : 0, s,
                        (running && p && running == p) ? "true" : "false");
    }
    len += snprintf(buf + len, sizeof(buf) - len, "]}");
    request->send(200, "application/json", buf);
}

void OtaManager::handleStart(AsyncWebServerRequest* request) {
    String slot = request->getParam("slot") ? request->getParam("slot")->value() : "";
    unsigned long size = request->getParam("size") ? (unsigned long)request->getParam("size")->value().toInt() : 0;

    if (xSemaphoreTake(otaMutex, portMAX_DELAY)) {
        // Self-heal a session stuck in Uploading (client vanished and no
        // further chunks arrived to trip the in-stream timeout).
        if (state == OtaState::Uploading && millis() - lastChunkMs > OTA_CHUNK_TIMEOUT_MS) {
            esp_ota_abort(otaHandle);
            state = OtaState::Idle;
            Serial.println("OTA: idle timeout, discarding stuck upload session");
        }
        if (state != OtaState::Idle) {
            xSemaphoreGive(otaMutex);
            request->send(409, "application/json", "{\"error\":\"ota_already_in_progress\"}");
            return;
        }

        const esp_partition_t* targetPart = findSlot(slot);
        if (!targetPart) {
            xSemaphoreGive(otaMutex);
            request->send(400, "application/json", "{\"error\":\"unknown_slot\"}");
            return;
        }

        const esp_partition_t* running = esp_ota_get_running_partition();
        if (targetPart == running) {
            xSemaphoreGive(otaMutex);
            request->send(400, "application/json",
                "{\"error\":\"cannot_update_running_slot\","
                "\"message\":\"This is the running firmware. Boot the other app and update from there.\"}");
            return;
        }

        if (size == 0 || size > targetPart->size) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "{\"error\":\"file_too_large\",\"size\":%lu,\"max\":%u}",
                     (unsigned long)size, (unsigned)targetPart->size);
            xSemaphoreGive(otaMutex);
            request->send(400, "application/json", msg);
            return;
        }

        esp_err_t err = esp_ota_begin(targetPart, OTA_WITH_SEQUENTIAL_WRITES, &otaHandle);
        if (err != ESP_OK) {
            char msg[96];
            snprintf(msg, sizeof(msg), "{\"error\":\"ota_begin_failed\",\"code\":%d}", (int)err);
            xSemaphoreGive(otaMutex);
            request->send(500, "application/json", msg);
            return;
        }

        target = targetPart;
        received = 0;
        expected = size;
        lastChunkMs = millis();
        state = OtaState::Uploading;
        xSemaphoreGive(otaMutex);

        Serial.printf("OTA start: %s (%lu bytes into %u byte slot)\n",
                      slot.c_str(), (unsigned long)size, (unsigned)targetPart->size);
        request->send(200, "application/json",
                      "{\"status\":\"ota_started\",\"slot\":\"" + String(slot) + "\"}");
    }
}

void OtaManager::otaBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    (void)total;
    if (!xSemaphoreTake(otaMutex, portMAX_DELAY)) return;

    // Already failed this POST, or the session is gone: drop the rest.
    if (otaErrCode != 0 || state != OtaState::Uploading) {
        if (otaErrCode == 0) {
            otaErrCode = 409;
            otaErrMsg = "{\"error\":\"not_uploading\"}";
        }
        xSemaphoreGive(otaMutex);
        return;
    }

    if (index == 0) {
        // First byte of this POST: accept only a raw binary stream. Form and
        // multipart bodies never reach here (trivial handler) — otaDone
        // rejects them via the content type. text/* bodies do arrive, so
        // reject them before the first flash write.
        String ct = request->contentType();
        if (ct.length() && ct != "application/octet-stream" && ct != "application/binary") {
            otaErrCode = 415;
            otaErrMsg = "{\"error\":\"unsupported_media_type\","
                        "\"message\":\"send raw bytes with Content-Type: application/octet-stream\"}";
            xSemaphoreGive(otaMutex);
            return;
        }
        // Idle timeout: client vanished mid-upload.
        if (millis() - lastChunkMs > OTA_CHUNK_TIMEOUT_MS) {
            esp_ota_abort(otaHandle);
            state = OtaState::Idle;
            otaErrCode = 408;
            otaErrMsg = "{\"error\":\"upload_timeout\"}";
            xSemaphoreGive(otaMutex);
            return;
        }
    }

    esp_err_t err = esp_ota_write(otaHandle, data, len);
    lastChunkMs = millis();
    if (err != ESP_OK) {
        esp_ota_abort(otaHandle);
        state = OtaState::Idle;
        otaErrCode = 500;
        char msg[96];
        snprintf(msg, sizeof(msg), "{\"error\":\"ota_write_failed\",\"code\":%d}", (int)err);
        otaErrMsg = msg;
        xSemaphoreGive(otaMutex);
        return;
    }
    received += len;
    xSemaphoreGive(otaMutex);
}

void OtaManager::otaDone(AsyncWebServerRequest* request) {
    if (!xSemaphoreTake(otaMutex, portMAX_DELAY)) return;

    int code = otaErrCode;
    String msg = otaErrMsg;
    otaErrCode = 0;
    otaErrMsg = "";

    // Form/multipart bodies are counted but never delivered to otaBody
    // (trivial handler) — reject them explicitly.
    if (code == 0) {
        String ct = request->contentType();
        if (ct.startsWith("application/x-www-form-urlencoded") || ct.startsWith("multipart/") ||
            ct == "text/plain") {
            code = 415;
            msg = "{\"error\":\"unsupported_media_type\","
                  "\"message\":\"send raw bytes with Content-Type: application/octet-stream\"}";
        }
    }

    bool uploading = (state == OtaState::Uploading);
    bool finished = uploading && received >= expected;
    unsigned rec = received;
    xSemaphoreGive(otaMutex);

    if (code != 0) {
        request->send(code, "application/json", msg);
        return;
    }
    if (!uploading) {
        request->send(409, "application/json", "{\"error\":\"not_uploading\"}");
        return;
    }
    if (finished) {
        Serial.printf("OTA upload complete: %u bytes, waiting for /finish\n", (unsigned)rec);
    }
    char body[96];
    snprintf(body, sizeof(body), "{\"status\":\"chunk_ok\",\"received\":%u,\"done\":%s}",
             rec, finished ? "true" : "false");
    request->send(200, "application/json", body);
}

void OtaManager::handleFinish(AsyncWebServerRequest* request) {
    if (xSemaphoreTake(otaMutex, portMAX_DELAY)) {
        if (state != OtaState::Uploading) {
            xSemaphoreGive(otaMutex);
            request->send(409, "application/json", "{\"error\":\"not_uploading\"}");
            return;
        }
        if (received != expected) {
            esp_ota_abort(otaHandle);
            state = OtaState::Idle;
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "{\"error\":\"incomplete_upload\",\"received\":%u,\"expected\":%u}",
                     (unsigned)received, (unsigned)expected);
            xSemaphoreGive(otaMutex);
            request->send(400, "application/json", msg);
            return;
        }

        // Validates the image (magic, segments, CRC) and closes the handle.
        // On IDF 5.x a successful esp_ota_end() also records the new image
        // in otadata with state NEW (rollback-verified on its first two boots).
        esp_err_t err = esp_ota_end(otaHandle);
        if (err != ESP_OK) {
            state = OtaState::Idle;
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "{\"error\":\"invalid_image\",\"code\":%d,"
                     "\"message\":\"The file is not a valid app image for this chip.\"}", (int)err);
            xSemaphoreGive(otaMutex);
            request->send(400, "application/json", msg);
            return;
        }

        state = OtaState::Ready;
        const char* label = target->label;
        xSemaphoreGive(otaMutex);

        Serial.printf("OTA ready: %s image validated, awaiting boot switch\n", label);
        request->send(200, "application/json",
                      "{\"status\":\"ota_ready\",\"slot\":\"" + String(label) + "\"}");
    }
}

void OtaManager::handleAbort(AsyncWebServerRequest* request) {
    if (xSemaphoreTake(otaMutex, portMAX_DELAY)) {
        if (state == OtaState::Uploading) {
            esp_ota_abort(otaHandle);
        }
        state = OtaState::Idle;
        xSemaphoreGive(otaMutex);
        Serial.println("OTA aborted");
        request->send(200, "application/json", "{\"status\":\"ota_aborted\"}");
    }
}

void OtaManager::handleBoot(AsyncWebServerRequest* request) {
    String slot = request->getParam("slot") ? request->getParam("slot")->value() : "";
    const esp_partition_t* targetPart = findSlot(slot);
    if (!targetPart) {
        request->send(400, "application/json", "{\"error\":\"unknown_slot\"}");
        return;
    }

    const esp_partition_t* running = esp_ota_get_running_partition();
    if (targetPart == running) {
        request->send(400, "application/json", "{\"error\":\"already_running_this_slot\"}");
        return;
    }

    // Refuse to boot a slot explicitly marked bad. UNDEFINED (esptool-flashed,
    // never written through esp_ota_end) is fine — esp_ota_set_boot_partition()
    // below validates the actual image header anyway.
    esp_ota_img_states_t imgState = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(targetPart, &imgState);
    if (imgState == ESP_OTA_IMG_INVALID || imgState == ESP_OTA_IMG_ABORTED) {
        request->send(400, "application/json", "{\"error\":\"image_marked_invalid\"}");
        return;
    }

    if (esp_ota_set_boot_partition(targetPart) != ESP_OK) {
        request->send(500, "application/json", "{\"error\":\"boot_switch_failed\"}");
        return;
    }

    Serial.printf("OTA: switching boot to %s, rebooting...\n", slot.c_str());
    request->send(200, "application/json", "{\"status\":\"rebooting\",\"slot\":\"" + String(slot) + "\"}");
    // Give the HTTP response time to flush before the reset.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

// ===== Data partition (SPIFFS) file ops =====

bool OtaManager::isValidDataPath(const String& pathIn, String& normalized) {
    normalized = pathIn;
    if (normalized.length() < 2 || normalized.length() > 128) return false;
    if (normalized[0] != '/') normalized = "/" + normalized;

    // Walk the path: no traversal, safe charset only.
    String seg;
    for (int i = 1; i < (int)normalized.length(); i++) {
        char c = normalized[i];
        if (c == '/') {
            if (seg.length() < 1 || seg == "." || seg == "..") return false;
            seg = "";
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_')) {
            return false;
        }
        seg += c;
    }
    if (seg.length() < 1 || seg == "." || seg == "..") return false;
    // Don't allow a file named just "." or hidden dotfiles at root
    if (normalized == "/.") return false;
    return true;
}

void OtaManager::dataBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    // Write every call's bytes unconditionally (onBody fires once per TCP
    // segment); the HTTP response is sent exactly once from dataDone().
    (void)total;
    dataErrCode = 0;
    dataErrMsg = "";

    if (!xSemaphoreTake(otaMutex, portMAX_DELAY)) return;
    if (index == 0) {
        // Start of a new file upload: reset any previous error, validate + open.
        dataErrCode = 0;
        dataErrMsg = "";
        if (dataWriting) {  // previous upload abandoned mid-stream — recover
            dataFile.close();
            dataWriting = false;
        }
        if (total == 0 || total > DATA_FILE_MAX_SIZE) {
            xSemaphoreGive(otaMutex);
            dataErrCode = 413; dataErrMsg = "file_too_large";
            return;
        }
        if (!SPIFFS.begin(false)) {
            xSemaphoreGive(otaMutex);
            dataErrCode = 500; dataErrMsg = "spiffs_not_mounted";
            return;
        }
        if ((size_t)(SPIFFS.totalBytes() - SPIFFS.usedBytes()) < total) {
            xSemaphoreGive(otaMutex);
            dataErrCode = 507; dataErrMsg = "not_enough_space";
            return;
        }
        dataFile = SPIFFS.open(dataPath.c_str(), "w");
        if (!dataFile) {
            xSemaphoreGive(otaMutex);
            dataErrCode = 500; dataErrMsg = "open_failed";
            return;
        }
        dataWriting = true;
        dataReceived = 0;
        dataExpected = total;
        xSemaphoreGive(otaMutex);
        return;
    }
    if (!dataWriting) {
        // Continuation of a POST we never started (client retried?) — reject.
        xSemaphoreGive(otaMutex);
        dataErrCode = 409; dataErrMsg = "no_active_write";
        return;
    }

    bool failed = !dataFile || dataFile.write(data, len) != (int)len;
    dataReceived += len;
    if (failed && dataErrCode == 0) { dataErrCode = 500; dataErrMsg = "write_failed"; }
    xSemaphoreGive(otaMutex);
}

void OtaManager::dataDone(AsyncWebServerRequest* request) {
    if (dataErrCode != 0) {
        xSemaphoreTake(otaMutex, portMAX_DELAY);
        if (dataWriting) { dataFile.close(); dataWriting = false; }
        xSemaphoreGive(otaMutex);
        request->send(dataErrCode, "application/json", "{\"error\":\"" + dataErrMsg + "\"}");
        return;
    }
    if (dataWriting) {
        xSemaphoreTake(otaMutex, portMAX_DELAY);
        dataFile.close();
        dataWriting = false;
        bool ok = dataReceived == dataExpected;
        String path = dataPath;
        size_t received = dataReceived;
        xSemaphoreGive(otaMutex);
        if (ok) {
            Serial.printf("Data file written: %s (%u bytes)\n", path.c_str(), (unsigned)received);
            request->send(200, "application/json",
                          "{\"status\":\"written\",\"path\":\"" + path.substring(1) + "\",\"size\":" + String((unsigned)received) + "}");
        } else {
            request->send(500, "application/json", "{\"error\":\"short_write\"}");
        }
        return;
    }
    // No active write: the body was consumed by the framework's form parser
    // (wrong content type) or the POST had an empty/invalid body. dataBody
    // never ran, so there is nothing on disk to clean up.
    request->send(415, "application/json", "{\"error\":\"unsupported_content_type\",\"hint\":\"Content-Type: application/octet-stream required\"}");
}

void OtaManager::handleDataDelete(AsyncWebServerRequest* request) {
    String pathParam = request->getParam("path") ? request->getParam("path")->value() : "";
    String path;
    if (!isValidDataPath(pathParam, path)) {
        request->send(400, "application/json", "{\"error\":\"invalid_path\"}");
        return;
    }
    if (!SPIFFS.begin(false)) {
        request->send(500, "application/json", "{\"error\":\"spiffs_not_mounted\"}");
        return;
    }
    if (!SPIFFS.exists(path)) {
        request->send(404, "application/json", "{\"error\":\"not_found\"}");
        return;
    }
    if (!SPIFFS.remove(path)) {
        request->send(500, "application/json", "{\"error\":\"delete_failed\"}");
        return;
    }
    Serial.printf("Data file deleted: %s\n", path.c_str());
    request->send(200, "application/json", "{\"status\":\"deleted\",\"path\":\"" + path.substring(1) + "\"}");
}
