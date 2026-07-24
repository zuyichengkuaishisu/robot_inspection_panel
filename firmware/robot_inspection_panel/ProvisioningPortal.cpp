#include "ProvisioningPortal.h"

#include <HTTPClient.h>

#define LOGI(format, ...) do {                          \
  if (Serial && Serial.availableForWrite() >= 128) {      \
    Serial.printf("[portal][%8lu] " format "\n",          \
                  millis(), ##__VA_ARGS__);               \
  }                                                       \
} while (0)

void ProvisioningPortal::begin(const char *fallbackBackend, const char *deviceId) {
  deviceId_ = deviceId;
  preferences_.begin("provisioning", false);
  bool hasSavedConfig = preferences_.getUChar("version", 0) == 1;
  ssid_ = hasSavedConfig ? preferences_.getString("ssid", "") : "";
  password_ = hasSavedConfig ? preferences_.getString("password", "") : "";
  backendUrl_ = hasSavedConfig ? preferences_.getString("backend", fallbackBackend) : String(fallbackBackend);
  while (backendUrl_.endsWith("/")) backendUrl_.remove(backendUrl_.length() - 1);

  if (ssid_.length() > 0) {
    // Saved Wi-Fi exists — try connecting directly
    LOGI("Saved Wi-Fi found: %s, connecting...", ssid_.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid_.c_str(), password_.c_str());
    // We'll connect in loop(), show scanning state briefly
    setState(PROVISION_SCANNING, "正在连接 " + ssid_);
    return;
  }

  // No saved config — scan for 8s then go to AP mode
  LOGI("No saved Wi-Fi, scanning for 8s...");
  WiFi.mode(WIFI_STA);
  WiFi.scanNetworks(true, true);
  scanInProgress_ = true;
  stateSince_ = millis();
  setState(PROVISION_SCANNING, "正在扫描 Wi-Fi");
}

void ProvisioningPortal::loop() {
  // Handle DNS + web server when in portal mode
  if (isPortal()) {
    dns_.processNextRequest();
    server_.handleClient();
  }

  // Scan in progress
  if (scanInProgress_) {
    int16_t result = WiFi.scanComplete();
    if (result >= 0 || result == WIFI_SCAN_FAILED || millis() - stateSince_ >= SCAN_TIMEOUT_MS) {
      finishScan(result >= 0 ? result : 0);
    }
    return;
  }

  // If we had saved config, check connection progress
  // (entered this state from begin() with saved credentials)
  if (state_ == PROVISION_SCANNING && ssid_.length() > 0) {
    if (WiFi.status() == WL_CONNECTED) {
      LOGI("Connected to saved Wi-Fi: %s, IP=%s", ssid_.c_str(), WiFi.localIP().toString().c_str());
      setState(PROVISION_CONNECTING, "Wi-Fi 已连接");
      // Give rendering time, then auto-proceed to connected state
      connectAndEnterNormal();
    } else if (millis() - stateSince_ > 12000) {
      // Failed to connect — scan then go to AP
      LOGI("Saved Wi-Fi connection failed, scanning...");
      WiFi.scanNetworks(true, true);
      scanInProgress_ = true;
      stateSince_ = millis();
      setState(PROVISION_SCANNING, "正在扫描 Wi-Fi");
    }
    return;
  }

  // Verifying credentials from portal
  if (state_ == PROVISION_VERIFYING) {
    verifyCredentials();
  }

  // Success — restart after delay
  if (state_ == PROVISION_CONNECTING && restartAt_ > 0 && static_cast<int32_t>(millis() - restartAt_) >= 0) {
    LOGI("Rebooting to apply new Wi-Fi config...");
    ESP.restart();
  }
}

void ProvisioningPortal::finishScan(int16_t count) {
  scanInProgress_ = false;
  networkCount_ = 0;

  for (int16_t i = 0; i < count && networkCount_ < MAX_NETWORKS; ++i) {
    String name = WiFi.SSID(i);
    if (!name.length()) continue;
    // Deduplicate by SSID, keep strongest RSSI
    int existing = -1;
    for (uint8_t j = 0; j < networkCount_; ++j) {
      if (networks_[j].ssid == name) { existing = j; break; }
    }
    if (existing >= 0) {
      if (WiFi.RSSI(i) > networks_[existing].rssi) networks_[existing].rssi = WiFi.RSSI(i);
      continue;
    }
    networks_[networkCount_++] = {name, WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN};
  }

  // Sort by RSSI descending
  for (uint8_t i = 0; i < networkCount_; ++i) {
    for (uint8_t j = i + 1; j < networkCount_; ++j) {
      if (networks_[j].rssi > networks_[i].rssi) {
        ProvisionNetwork item = networks_[i];
        networks_[i] = networks_[j];
        networks_[j] = item;
      }
    }
  }
  WiFi.scanDelete();

  // Log found networks
  for (uint8_t i = 0; i < networkCount_; ++i) {
    LOGI("  Network[%u]: %s RSSI=%d %s", i,
         networks_[i].ssid.c_str(), networks_[i].rssi,
         networks_[i].secured ? "secured" : "open");
  }

  if (isPortal()) {
    // Already in AP mode — just update the list
    message_ = "Wi-Fi 列表已更新";
    renderRequested_ = true;
    LOGI("Scan complete, %u networks found (portal mode)", networkCount_);
    return;
  }

  // First scan (boot) — if we have saved config already connected, don't start AP
  if (ssid_.length() > 0 && WiFi.status() == WL_CONNECTED) {
    connectAndEnterNormal();
    return;
  }

  // No saved config or failed — start AP mode
  LOGI("No usable Wi-Fi, starting AP mode...");
  startPortal();
}

void ProvisioningPortal::startPortal() {
  // Clean up any previous state
  WiFi.mode(WIFI_AP);
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, false);
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);

  IPAddress apIp(192, 168, 4, 1);
  IPAddress apMask(255, 255, 255, 0);
  WiFi.softAPConfig(apIp, apIp, apMask);

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
  apSsid_ = "SmartInspect-" + String(suffix);

  if (!WiFi.softAP(apSsid_.c_str(), nullptr, 1, false, 4)) {
    LOGI("ERROR: softAP failed!");
  } else {
    LOGI("AP started: %s IP=%s", apSsid_.c_str(), WiFi.softAPIP().toString().c_str());
  }

  // Try to stop/recreate DNS cleanly
  dns_.stop();
  dns_.start(53, "*", WiFi.softAPIP());

  setupRoutes();
  server_.begin();

  setState(PROVISION_AP, "请连接热点并打开配置页");
}

void ProvisioningPortal::requestScan() {
  if (scanInProgress_ || state_ == PROVISION_VERIFYING) return;
  WiFi.scanDelete();
  WiFi.scanNetworks(true, true);
  scanInProgress_ = true;
  stateSince_ = millis();
  message_ = "正在扫描 Wi-Fi";
}

void ProvisioningPortal::verifyCredentials() {
  static uint32_t verifyStart = 0;
  if (verifyStart == 0) {
    verifyStart = millis();
    LOGI("Verifying credentials for SSID=%s", pendingSsid_.c_str());
    // Temporarily connect to target Wi-Fi to test
    WiFi.mode(WIFI_STA);
    WiFi.begin(pendingSsid_.c_str(), pendingPassword_.c_str());
    backendHealthy_ = false;
  }

  if (WiFi.status() == WL_CONNECTED) {
    // Wi-Fi connected — now check backend
    LOGI("Target Wi-Fi connected, checking backend...");
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    String url = pendingBackend_;
    while (url.endsWith("/")) url.remove(url.length() - 1);
    url += "/health";
    if (http.begin(url)) {
      int code = http.GET();
      http.end();
      backendHealthy_ = (code == 200);
      LOGI("Backend health check: code=%d healthy=%d", code, (int)backendHealthy_);
    } else {
      LOGI("Backend health check: failed to begin request");
    }

    if (backendHealthy_) {
      LOGI("All good! Saving config and switching to STA...");
      // Save config
      saveConfig(pendingSsid_, pendingPassword_, pendingBackend_);
      setState(PROVISION_CONNECTING, "配置成功");
      // Wait a moment then restart
      restartAt_ = millis() + RESTART_DELAY_MS;
      verifyStart = 0;
      return;
    } else {
      // Backend unreachable — warn but still save and connect
      LOGI("Backend unreachable, saving Wi-Fi config anyway...");
      saveConfig(pendingSsid_, pendingPassword_, pendingBackend_);
      setState(PROVISION_CONNECTING, "Wi-Fi 已保存（后端不可用）");
      restartAt_ = millis() + RESTART_DELAY_MS;
      verifyStart = 0;
      return;
    }
  }

  if (millis() - verifyStart >= VERIFY_TIMEOUT_MS) {
    // Timeout — restore AP mode
    LOGI("Wi-Fi verification timeout for %s", pendingSsid_.c_str());
    WiFi.disconnect(true, false);
    setState(PROVISION_FAILED, "无法连接 " + pendingSsid_);
    verifyStart = 0;
    // Restart AP
    WiFi.mode(WIFI_AP);
    WiFi.softAP(apSsid_.c_str(), nullptr, 1, false, 4);
    dns_.start(53, "*", WiFi.softAPIP());
    setupRoutes();
    server_.begin();
  }
}

void ProvisioningPortal::saveConfig(const String &ssid, const String &password, const String &backend) {
  ssid_ = ssid;
  password_ = password;
  backendUrl_ = backend;
  while (backendUrl_.endsWith("/")) backendUrl_.remove(backendUrl_.length() - 1);
  preferences_.putString("ssid", ssid_);
  preferences_.putString("password", password_);
  preferences_.putString("backend", backendUrl_);
  preferences_.putUChar("version", 1);
  LOGI("Saved config to NVS: ssid=%s backend=%s", ssid_.c_str(), backendUrl_.c_str());
}

void ProvisioningPortal::connectAndEnterNormal() {
  LOGI("Entering normal operation mode, IP=%s", WiFi.localIP().toString().c_str());
  // We're already connected, just set state
  setState(PROVISION_CONNECTING, "Wi-Fi 已连接");
  // Stop DNS if it was running
  dns_.stop();
  server_.stop();
  routesReady_ = false;
  // The main loop will transition to normal after render
  restartAt_ = 0; // No restart needed
}

void ProvisioningPortal::setupRoutes() {
  if (routesReady_) return;
  routesReady_ = true;

  server_.on("/", HTTP_GET, [this]() { sendPortalPage(); });
  server_.on("/api/networks", HTTP_GET, [this]() { sendNetworks(); });
  server_.on("/api/status", HTTP_GET, [this]() { sendStatus(); });
  server_.on("/api/config", HTTP_GET, [this]() {
    JsonDocument doc;
    doc["backend_url"] = backendUrl_;
    doc["device_id"] = deviceId_;
    doc["ap_ssid"] = apSsid_;
    sendJson(200, doc);
  });
  server_.on("/api/rescan", HTTP_POST, [this]() {
    requestScan();
    server_.send(202, "application/json", "{\"ok\":true}");
  });

  // Catch-all for captive portal probes
  server_.onNotFound([this]() {
    sendRedirect();
  });

  // Handle connect/provision
  server_.on("/api/provision", HTTP_POST, [this]() { handleProvisionPost(); });
}

void ProvisioningPortal::handleProvisionPost() {
  if (!server_.hasArg("plain")) {
    server_.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"请提供 Wi-Fi 配置\"}");
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server_.arg("plain"));
  if (err) { sendJson(400, doc); doc["ok"] = false; doc["error"] = "无效的配置格式"; return; }
  String ssid = doc["ssid"] | "";
  String password = doc["password"] | "";
  String backend = doc["backend_url"] | "";
  if (ssid.length() < 1 || ssid.length() > 32) { doc["ok"] = false; doc["error"] = "Wi-Fi 名称应为 1-32 个字符"; sendJson(400, doc); return; }
  if (password.length() > 0 && (password.length() < 8 || password.length() > 63)) { doc["ok"] = false; doc["error"] = "密码应为 8-63 个字符或留空"; sendJson(400, doc); return; }
  if (!validBackend(backend)) { doc["ok"] = false; doc["error"] = "后端地址必须以 http:// 开头"; sendJson(400, doc); return; }

  // Store pending credentials
  pendingSsid_ = ssid;
  pendingPassword_ = password;
  pendingBackend_ = backend;

  doc["ok"] = true;
  sendJson(200, doc);

  LOGI("Provision post: ssid=%s backend=%s", ssid.c_str(), backend.c_str());

  // Begin verification (will switch to STA mode to test)
  setState(PROVISION_VERIFYING, "正在验证 Wi-Fi");
}

void ProvisioningPortal::sendPortalPage() {
  server_.sendHeader("Cache-Control", "no-store");
  server_.sendHeader("Connection", "close");
  server_.send(200, "text/html; charset=utf-8", html());
}

void ProvisioningPortal::sendJson(int code, JsonDocument &doc) {
  String body;
  serializeJson(doc, body);
  server_.sendHeader("Connection", "close");
  server_.send(code, "application/json; charset=utf-8", body);
}

void ProvisioningPortal::sendNetworks() {
  JsonDocument doc;
  JsonArray list = doc["networks"].to<JsonArray>();
  for (uint8_t i = 0; i < networkCount_; ++i) {
    JsonObject item = list.add<JsonObject>();
    item["ssid"] = networks_[i].ssid;
    item["rssi"] = networks_[i].rssi;
    item["secured"] = networks_[i].secured;
  }
  sendJson(200, doc);
}

void ProvisioningPortal::sendStatus() {
  JsonDocument doc;
  doc["state"] = stateName();
  doc["message"] = message_;
  doc["ssid"] = pendingSsid_;
  doc["backend_healthy"] = backendHealthy_;
  sendJson(200, doc);
}

void ProvisioningPortal::sendRedirect() {
  server_.sendHeader("Connection", "close");
  server_.sendHeader("Location", "http://192.168.4.1/", true);
  server_.send(302, "text/plain; charset=utf-8", "配置地址：http://192.168.4.1/");
}

bool ProvisioningPortal::validBackend(const String &value) const {
  return value.length() >= 8 && value.length() <= 160 && value.startsWith("http://") && value.indexOf(' ') < 0;
}

void ProvisioningPortal::setState(ProvisioningState next, const String &message) {
  state_ = next;
  stateSince_ = millis();
  message_ = message;
  renderRequested_ = true;
}

String ProvisioningPortal::stateName() const {
  switch (state_) {
    case PROVISION_SCANNING:   return "scanning";
    case PROVISION_AP:         return "ap";
    case PROVISION_VERIFYING:  return "verifying";
    case PROVISION_CONNECTING: return "connecting";
    case PROVISION_FAILED:     return "failed";
    case PROVISION_SUCCESS:    return "success";
  }
  return "unknown";
}

String ProvisioningPortal::html() const {
  return R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>智巡精灵网络配置</title>
<style>body{font-family:system-ui,sans-serif;background:#101417;color:#f4f7f8;margin:0;padding:20px}main{max-width:520px;margin:auto}h1{font-size:24px}label{display:block;margin:14px 0 6px}input,button{box-sizing:border-box;width:100%;padding:12px;border-radius:8px;border:1px solid #52616b;font-size:16px}input{background:#1b2328;color:#fff}button{background:#168d91;color:#fff;border:0;margin-top:16px;cursor:pointer}.secondary{background:#34434b}.network{display:flex;justify-content:space-between;padding:11px 0;border-bottom:1px solid #34434b;cursor:pointer}.network.selected{background:#1b3a3c;border-color:#168d91}.muted{color:#9eabb2}.status{margin:16px 0;padding:12px;background:#1b2328;border-radius:8px;word-break:break-all}.result{margin:12px 0;padding:10px;border-radius:6px;display:none}.result.success{display:block;background:#0d3320;color:#8bc34a}.result.error{display:block;background:#330d0d;color:#ef5350}.manual-toggle{color:#168d91;cursor:pointer;margin-top:8px;display:inline-block}</style></head>
<body><main><h1>智巡精灵 v1.1</h1><p class="muted">配置设备连接的工作网络</p><div id="status" class="status">正在读取状态...</div><button class="secondary" id="scanBtn">刷新 Wi-Fi 列表</button><div id="nets"></div><div class="muted manual-toggle" id="manualToggle">手动输入 Wi-Fi 名称</div><div id="manualInput" style="display:none"><label>Wi-Fi 名称</label><input id="ssid" maxlength="32" autocomplete="off"></div><label>Wi-Fi 密码（开放网络留空）</label><input id="password" type="password" maxlength="63" autocomplete="new-password"><label>后端地址</label><input id="backend" maxlength="160"><button id="saveBtn">连接 Wi-Fi</button><div id="result" class="result"></div></main>
<script>const $=id=>document.getElementById(id);let selSsid=null;async function getStatus(){try{const d=await(await fetch('/api/status')).json();$('status').innerHTML='<strong>设备状态：</strong>'+d.message;$('#result').className='result'}catch(e){}}async function load(){try{const cfg=await(await fetch('/api/config')).json();if(!$('backend').value)$('backend').value=cfg.backend_url||'http://192.168.0.35:8765';const d=await(await fetch('/api/networks')).json();const root=$('nets');root.textContent='';d.networks.forEach(n=>{const row=document.createElement('div');row.className='network';const name=document.createElement('span');name.textContent=n.ssid;const meta=document.createElement('span');meta.textContent=n.rssi+' dBm '+(n.secured?'加密':'开放');row.append(name,meta);row.onclick=()=>{document.querySelectorAll('.network').forEach(r=>r.classList.remove('selected'));row.classList.add('selected');$('ssid').value=n.ssid;selSsid=n.ssid};root.append(row)})}catch(e){console.error(e)}getStatus()}$('scanBtn').onclick=async()=>{$('result').className='result';$('result').textContent='正在扫描...';await fetch('/api/rescan',{method:'POST'});setTimeout(load,2000)};$('manualToggle').onclick=()=>{const m=$('manualInput');m.style.display=m.style.display==='none'?'block':'none'};$('saveBtn').onclick=async()=>{const ssid=$('ssid').value.trim();if(!ssid){$('result').className='result error';$('result').textContent='请输入 Wi-Fi 名称';return}const body={ssid:ssid,password:$('password').value,backend_url:$('backend').value};$('result').className='result';$('result').textContent='正在连接 Wi-Fi，请稍候...';try{const r=await fetch('/api/provision',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});const d=await r.json();if(d.ok){$('result').className='result success';$('result').textContent='正在连接，请稍候...';getStatus()}else{$('result').className='result error';$('result').textContent=d.error||'提交失败'}}catch(e){$('result').className='result error';$('result').textContent='网络错误：'+e.message}};load()</script></body></html>)HTML";
}
