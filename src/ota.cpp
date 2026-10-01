#ifndef INTEGRATED  // The integrated S3 build replaces the old web API / OTA updater with the SFOS-ported web stack
#include "ota.h"

#define OTA_CHUNK_TIMEOUT_MS 120000UL

OtaUpdater::OtaUpdater() {
  mutex = xSemaphoreCreateMutex();
}

void OtaUpdater::begin(AsyncWebServer *server) {
  server->on("/", HTTP_GET, [this](AsyncWebServerRequest *r) { handleRoot(r); });
  server->on("/api/ota/status", HTTP_GET, [this](AsyncWebServerRequest *r) { handleStatus(r); });
  server->on("/api/ota/start", HTTP_POST, [this](AsyncWebServerRequest *r) { handleStart(r); });
  // Raw-body upload: callers MUST send Content-Type: application/octet-stream.
  // OtaRawBodyHandler makes the route "trivial" so form-encoded/multipart bodies
  // are never parsed into RAM (the framework's form parser accumulates the
  // whole body and can OOM the board); octet-stream bytes still stream in
  // per-chunk and otaDone() sends exactly one response per completed POST.
  server->addHandler(new OtaRawBodyHandler(
    "/api/ota/data",
    [this](AsyncWebServerRequest *r, uint8_t *d, size_t l, size_t i, size_t t) { otaBody(r, d, l, i, t); },
    [this](AsyncWebServerRequest *r) { otaDone(r); }));
  server->on("/api/ota/finish", HTTP_POST, [this](AsyncWebServerRequest *r) { handleFinish(r); });
  server->on("/api/ota/abort", HTTP_POST, [this](AsyncWebServerRequest *r) { handleAbort(r); });
  server->on("/api/ota/boot", HTTP_POST, [this](AsyncWebServerRequest *r) { handleBoot(r); });
}

const char *OtaUpdater::stateName(State s) {
  switch (s) {
  case State::Idle: return "idle";
  case State::Uploading: return "uploading";
  case State::Ready: return "ready";
  }
  return "idle";
}

static const esp_partition_t *findOtaSlot(const char *label) {
  // Whitelist: only the two app slots may be addressed by name.
  // NB: use the exact OTA subtypes — in this IDF, subtype 0 is NOT "any"
  // (ESP_PARTITION_SUBTYPE_ANY is 0xFF), so a 0 cast silently finds nothing.
  if (strcmp(label, "ota_0") == 0)
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0");
  if (strcmp(label, "ota_1") == 0)
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, "ota_1");
  return nullptr;
}

void OtaUpdater::handleRoot(AsyncWebServerRequest *request) {
  // Minimal update page (the NuclearCounter app itself has no web UI; this
  // page exists so the StarForgeOS slot can be updated over WiFi).
  static const char PAGE[] =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>NuclearCounter — Update</title><style>"
    "body{font-family:'Courier New',monospace;background:#0a0e27;color:#e0e6ed;"
    "display:flex;justify-content:center;padding:24px;margin:0}"
    ".card{background:linear-gradient(145deg,#1a1f35,#141824);border:2px solid #2a3550;"
    "border-radius:12px;padding:28px;max-width:520px;width:100%}"
    "h1{color:#ff7b00;font-size:20px;letter-spacing:2px;margin:0 0 4px}"
    "h2{font-size:13px;color:#8b9dc3;font-weight:400;margin:0 0 20px}"
    ".status{background:#10141f;border:1px solid #2a3550;border-radius:8px;"
    "padding:10px 14px;font-size:12px;color:#aabbdd;margin-bottom:16px;line-height:1.6}"
    "input[type=file]{width:100%;color:#8b9dc3;margin-bottom:12px}"
    ".row{display:flex;gap:10px;align-items:center;margin-bottom:10px}"
    "button{flex:1;font-family:inherit;font-size:14px;font-weight:700;letter-spacing:1px;"
    "padding:12px;border-radius:8px;border:2px solid #00ff88;cursor:pointer}"
    "#installBtn{background:linear-gradient(145deg,#00ff88,#00cc66);color:#0a0e27}"
    "#bootBtn{background:linear-gradient(145deg,#ff7b00,#cc6200);border-color:#ff9933;color:#0a0e27;display:none}"
    "button:disabled{opacity:.4;cursor:not-allowed}"
    ".track{height:18px;background:#0d1120;border:1px solid #2a3550;border-radius:9px;"
    "overflow:hidden;display:none;margin-bottom:8px}"
    ".fill{height:100%;width:0%;background:linear-gradient(90deg,#00cc66,#00ff88);transition:width .15s}"
    "#progTxt{font-size:12px;color:#00ff88;text-align:right;display:none}"
    ".note{font-size:11px;color:#6b7a99;line-height:1.6;margin-top:14px}"
    "#msg{font-size:12px;margin-top:10px;min-height:16px}"
    "#msg.err{color:#ff5555}#msg.ok{color:#00ff88}"
    "</style></head><body><div class='card'>"
    "<h1>NuclearCounter</h1><h2>StarForgeOS (ota_1) firmware update</h2>"
    "<div class='status' id='status'>Loading...</div>"
    "<input type='file' id='file' accept='.bin'>"
    "<div class='row'><button id='installBtn' onclick='install()'>Install</button>"
    "<button id='bootBtn' onclick='boot()'></button></div>"
    "<div class='track' id='track'><div class='fill' id='fill'></div></div>"
    "<div id='progTxt'></div><div id='msg'></div>"
    "<p class='note'>Installs the selected .bin into the StarForgeOS slot while "
    "NuclearCounter keeps running. Nothing changes until you press the reboot "
    "button. Keep power connected. The .bin comes from the CI artifact "
    "(starforge-ota1.bin for your board).</p>"
    "</div><script>"
    "let data=null,busy=false,readySlot=null;"
    "function $(id){return document.getElementById(id)}"
    "function msg(t,cls){const m=$(\"msg\");m.textContent=t;m.className=cls||''}"
    "async function loadStatus(){"
    " try{const r=await fetch('/api/ota/status');data=await r.json();"
    "  readySlot=null;"
    "  let html='Running: <b>'+data.running+'</b><br>';"
    "  for(const s of data.slots){"
    "   const tag=s.running?'RUNNING':(s.image==='new'||s.image==='pending_verify')?'READY':'idle';"
    "   html+=s.label+': image '+s.image+' · max '+(s.size/1048576).toFixed(1)+' MB · '+tag+'<br>';"
    "   if(!s.running&&(s.image==='new'||s.image==='pending_verify'))readySlot=s.label;"
    "  }"
    "  $(\"status\").innerHTML=html;"
    "  const b=$(\"bootBtn\");"
    "  if(readySlot&&!busy){b.style.display='block';b.textContent='Reboot into '+readySlot}"
    "  else b.style.display='none';"
    " }catch(e){msg('Status load failed: '+e,\"err\")}"
    "}"
    "async function install(){"
    " if(!data||busy)return;"
    " const f=$(\"file\").files[0];"
    " if(!f){msg('Choose a .bin file first',\"err\");return}"
    " const slot=data.slots.find(s=>!s.running);"
    " if(!slot){msg('No updatable slot',\"err\");return}"
    " if(f.size>slot.size){msg('File too large ('+(f.size/1048576).toFixed(2)+' MB > '+(slot.size/1048576).toFixed(2)+' MB)',\"err\");return}"
    " if(!confirm('Install '+f.name+' ('+(f.size/1048576).toFixed(2)+' MB) into '+slot.label+'?\\n\\nNuclearCounter keeps running until you press the reboot button.'))return;"
    " busy=true;$(\"installBtn\").disabled=true;$(\"track\").style.display='block';$(\"progTxt\").style.display='block';"
    " try{"
    "  let r=await fetch('/api/ota/start?slot='+slot.label+'&size='+f.size);let j=await r.json();"
    "  if(!r.ok)throw new Error(j.error||r.status);"
    "  const C=65536;"
    "  for(let o=0;o<f.size;o+=C){"
    "   const b=f.slice(o,o+C);"
    "   r=await fetch('/api/ota/data',{method:'POST',body:b,headers:{'Content-Type':'application/octet-stream'}});j=await r.json().catch(()=>({}));"
    "   if(!r.ok)throw new Error(j.error||r.status);"
    "   const p=Math.min(100,(Math.min(f.size,o+b.size)/f.size)*100);"
    "   $(\"fill\").style.width=p.toFixed(1)+'%';"
    "   $(\"progTxt\").textContent=(Math.min(f.size,o+b.size)/1048576).toFixed(2)+' / '+(f.size/1048576).toFixed(2)+' MB';"
    "  }"
    "  r=await fetch('/api/ota/finish',{method:'POST'});j=await r.json().catch(()=>({}));"
    "  if(!r.ok)throw new Error(j.error||r.status);"
    "  msg('Installed — ready to boot','ok');"
    " }catch(e){msg('Update failed: '+e,\"err\");try{await fetch('/api/ota/abort',{method:'POST'})}catch(_){}}"
    " finally{busy=false;$(\"installBtn\").disabled=false;$(\"track\").style.display='none';$(\"progTxt\").style.display='none';loadStatus()}"
    "}"
    "async function boot(){"
    " if(!readySlot||busy)return;"
    " if(!confirm('Reboot into '+readySlot+' (StarForgeOS) now?'))return;"
    " try{const r=await fetch('/api/ota/boot?slot='+readySlot,{method:'POST'});"
    "  const j=await r.json().catch(()=>({}));if(!r.ok){msg('Boot failed: '+(j.error||r.status),\"err\");return}"
    " }catch(e){/* reboot drops the connection — expected */}"
    " msg('Rebooting into '+readySlot+' ...','ok');"
    " const t0=Date.now();const iv=setInterval(async()=>{"
    "  let up=false;try{up=(await fetch('/api/status')).ok}catch(_){}"
    "  if(up||Date.now()-t0>120000){clearInterval(iv);location.reload()}},2000);"
    "}"
    "loadStatus();"
    "</script></body></html>";
  request->send(200, "text/html", PAGE);
}

void OtaUpdater::handleStatus(AsyncWebServerRequest *request) {
  const esp_partition_t *running = esp_ota_get_running_partition();

  char buf[768];
  int len = snprintf(
    buf, sizeof(buf),
    "{\"running\":\"%s\",\"upload\":{\"state\":\"%s\",\"received\":%u,\"expected\":%u},\"slots\":[",
    running ? running->label : "?", stateName(state), (unsigned)received, (unsigned)expected);

  for (const char *label : {"ota_0", "ota_1"}) {
    const esp_partition_t *p = findOtaSlot(label);
    esp_ota_img_states_t img = ESP_OTA_IMG_UNDEFINED;
    if (p) esp_ota_get_state_partition(p, &img);
    const char *s = img == ESP_OTA_IMG_NEW ? "new"
                  : img == ESP_OTA_IMG_PENDING_VERIFY ? "pending_verify"
                  : img == ESP_OTA_IMG_VALID ? "valid"
                  : img == ESP_OTA_IMG_INVALID ? "invalid"
                  : img == ESP_OTA_IMG_ABORTED ? "aborted" : "undefined";
    if (len > 0) len += snprintf(buf + len, sizeof(buf) - len, ",");
    len += snprintf(
      buf + len, sizeof(buf) - len,
      "{\"label\":\"%s\",\"size\":%u,\"image\":\"%s\",\"running\":%s}",
      label, p ? (unsigned)p->size : 0, s, (running && p && running == p) ? "true" : "false");
  }
  len += snprintf(buf + len, sizeof(buf) - len, "]}");
  request->send(200, "application/json", buf);
}

void OtaUpdater::handleStart(AsyncWebServerRequest *request) {
  String slot = request->getParam("slot") ? request->getParam("slot")->value() : "";
  unsigned long size =
    request->getParam("size") ? (unsigned long)request->getParam("size")->value().toInt() : 0;

  if (xSemaphoreTake(mutex, portMAX_DELAY)) {
    // Self-heal a session stuck in Uploading (client vanished and no further
    // chunks arrived to trip the in-stream timeout).
    if (state == State::Uploading && millis() - lastChunkMs > OTA_CHUNK_TIMEOUT_MS) {
      esp_ota_abort(handle);
      state = State::Idle;
      Serial.println("OTA: idle timeout, discarding stuck upload session");
    }
    if (state != State::Idle) {
      xSemaphoreGive(mutex);
      request->send(409, "application/json", "{\"error\":\"ota_already_in_progress\"}");
      return;
    }
    const esp_partition_t *t = findOtaSlot(slot.c_str());
    if (!t) {
      xSemaphoreGive(mutex);
      request->send(400, "application/json", "{\"error\":\"unknown_slot\"}");
      return;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (t == running) {
      xSemaphoreGive(mutex);
      request->send(400, "application/json", "{\"error\":\"cannot_update_running_slot\"}");
      return;
    }
    if (size == 0 || size > t->size) {
      char m[128];
      snprintf(m, sizeof(m), "{\"error\":\"file_too_large\",\"size\":%lu,\"max\":%u}",
               (unsigned long)size, (unsigned)t->size);
      xSemaphoreGive(mutex);
      request->send(400, "application/json", m);
      return;
    }
    esp_err_t err = esp_ota_begin(t, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
      char m[96];
      snprintf(m, sizeof(m), "{\"error\":\"ota_begin_failed\",\"code\":%d}", (int)err);
      xSemaphoreGive(mutex);
      request->send(500, "application/json", m);
      return;
    }
    target = t;
    received = 0;
    expected = size;
    lastChunkMs = millis();
    state = State::Uploading;
    xSemaphoreGive(mutex);
    Serial.printf("OTA start: %s (%lu bytes)\n", slot.c_str(), (unsigned long)size);
    request->send(200, "application/json", "{\"status\":\"ota_started\"}");
  }
}

void OtaUpdater::otaBody(AsyncWebServerRequest *request, uint8_t *data, size_t len,
                         size_t index, size_t total) {
  // Write every call's bytes unconditionally (handleBody fires once per TCP
  // segment); the HTTP response is sent exactly once from otaDone().
  (void)total;
  if (!xSemaphoreTake(mutex, portMAX_DELAY)) return;

  // Already failed this POST, or the session is gone: drop the rest.
  if (errCode != 0 || state != State::Uploading) {
    if (errCode == 0) {
      errCode = 409;
      errMsg = "{\"error\":\"not_uploading\"}";
    }
    xSemaphoreGive(mutex);
    return;
  }

  if (index == 0) {
    // First byte of this POST: accept only a raw binary stream. Form and
    // multipart bodies never reach here (trivial handler) — otaDone rejects
    // them via the content type. text/* bodies do arrive, so reject them
    // before the first flash write.
    String ct = request->contentType();
    if (ct.length() && ct != "application/octet-stream" && ct != "application/binary") {
      errCode = 415;
      errMsg = "{\"error\":\"unsupported_media_type\","
               "\"message\":\"send raw bytes with Content-Type: application/octet-stream\"}";
      xSemaphoreGive(mutex);
      return;
    }
    // Idle timeout: client vanished mid-upload.
    if (millis() - lastChunkMs > OTA_CHUNK_TIMEOUT_MS) {
      esp_ota_abort(handle);
      state = State::Idle;
      errCode = 408;
      errMsg = "{\"error\":\"upload_timeout\"}";
      xSemaphoreGive(mutex);
      return;
    }
  }

  esp_err_t err = esp_ota_write(handle, data, len);
  lastChunkMs = millis();
  if (err != ESP_OK) {
    esp_ota_abort(handle);
    state = State::Idle;
    errCode = 500;
    char m[96];
    snprintf(m, sizeof(m), "{\"error\":\"ota_write_failed\",\"code\":%d}", (int)err);
    errMsg = m;
    xSemaphoreGive(mutex);
    return;
  }

  received += len;
  xSemaphoreGive(mutex);
}

void OtaUpdater::otaDone(AsyncWebServerRequest *request) {
  if (!xSemaphoreTake(mutex, portMAX_DELAY)) return;

  int code = errCode;
  String msg = errMsg;
  errCode = 0;
  errMsg = "";

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

  bool uploading = (state == State::Uploading);
  bool finished = uploading && received >= expected;
  unsigned rec = received;
  xSemaphoreGive(mutex);

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
  char m[96];
  snprintf(m, sizeof(m), "{\"status\":\"chunk_ok\",\"received\":%u,\"done\":%s}", rec,
           finished ? "true" : "false");
  request->send(200, "application/json", m);
}

void OtaUpdater::handleFinish(AsyncWebServerRequest *request) {
  if (xSemaphoreTake(mutex, portMAX_DELAY)) {
    if (state != State::Uploading) {
      xSemaphoreGive(mutex);
      request->send(409, "application/json", "{\"error\":\"not_uploading\"}");
      return;
    }
    if (received != expected) {
      esp_ota_abort(handle);
      state = State::Idle;
      char m[96];
      snprintf(m, sizeof(m), "{\"error\":\"incomplete_upload\",\"received\":%u,\"expected\":%u}",
               (unsigned)received, (unsigned)expected);
      xSemaphoreGive(mutex);
      request->send(400, "application/json", m);
      return;
    }
    esp_err_t err = esp_ota_end(handle);
    if (err != ESP_OK) {
      state = State::Idle;
      xSemaphoreGive(mutex);
      request->send(400, "application/json",
                    "{\"error\":\"invalid_image\","
                    "\"message\":\"Not a valid app image for this chip.\"}");
      return;
    }
    state = State::Ready;
    xSemaphoreGive(mutex);
    Serial.println("OTA ready: awaiting boot switch");
    request->send(200, "application/json", "{\"status\":\"ota_ready\"}");
  }
}

void OtaUpdater::handleAbort(AsyncWebServerRequest *request) {
  if (xSemaphoreTake(mutex, portMAX_DELAY)) {
    if (state == State::Uploading) esp_ota_abort(handle);
    state = State::Idle;
    xSemaphoreGive(mutex);
    request->send(200, "application/json", "{\"status\":\"ota_aborted\"}");
  }
}

void OtaUpdater::handleBoot(AsyncWebServerRequest *request) {
  String slot = request->getParam("slot") ? request->getParam("slot")->value() : "";
  const esp_partition_t *t = findOtaSlot(slot.c_str());
  if (!t) {
    request->send(400, "application/json", "{\"error\":\"unknown_slot\"}");
    return;
  }
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (t == running) {
    request->send(400, "application/json", "{\"error\":\"already_running_this_slot\"}");
    return;
  }
  esp_ota_img_states_t img = ESP_OTA_IMG_UNDEFINED;
  esp_ota_get_state_partition(t, &img);
  if (img == ESP_OTA_IMG_INVALID || img == ESP_OTA_IMG_ABORTED) {
    request->send(400, "application/json", "{\"error\":\"image_marked_invalid\"}");
    return;
  }
  if (esp_ota_set_boot_partition(t) != ESP_OK) {
    request->send(500, "application/json", "{\"error\":\"boot_switch_failed\"}");
    return;
  }
  Serial.printf("OTA: switching boot to %s, rebooting...\n", slot.c_str());
  request->send(200, "application/json", "{\"status\":\"rebooting\"}");
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
}
#endif  // INTEGRATED
