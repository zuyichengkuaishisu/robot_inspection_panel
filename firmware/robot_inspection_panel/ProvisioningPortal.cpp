#include "ProvisioningPortal.h"

#include <HTTPClient.h>

void ProvisioningPortal::begin(const char *fallbackSsid, const char *fallbackPassword, const char *fallbackBackend, const char *deviceId) {
  deviceId_ = deviceId;
  preferences_.begin("provisioning", false);
  bool hasSavedConfiguration = preferences_.getUChar("version", 0) == 1;
  ssid_ = hasSavedConfiguration ? preferences_.getString("ssid", "") : String(fallbackSsid);
  password_ = hasSavedConfiguration ? preferences_.getString("password", "") : String(fallbackPassword);
  backendUrl_ = hasSavedConfiguration ? preferences_.getString("backend", fallbackBackend) : String(fallbackBackend);
  if (ssid_ == "YOUR_WIFI_SSID" || ssid_ == "") ssid_ = "";
  if (password_ == "YOUR_WIFI_PASSWORD") password_ = "";
  while (backendUrl_.endsWith("/")) backendUrl_.remove(backendUrl_.length() - 1);
  if (preferences_.getBool("portal", false)) {
    startPortal();
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.scanNetworks(true, true);
  scanInProgress_ = true;
  stateSince_ = millis();
}

void ProvisioningPortal::loop() {
  if (isPortal()) {
    dns_.processNextRequest();
    server_.handleClient();
  }
  if (scanInProgress_) {
    int16_t result = WiFi.scanComplete();
    if (result >= 0 || result == WIFI_SCAN_FAILED || millis() - stateSince_ > 12000) finishScan(result >= 0 ? result : 0);
    return;
  }
  if (state_ == PROVISION_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) setState(PROVISION_NORMAL, "Wi-Fi 已连接");
    else if (millis() - stateSince_ >= CONNECT_TIMEOUT_MS) startPortal();
  } else if (state_ == PROVISION_VERIFYING) {
    verifyProvisioningConnection();
  } else if (state_ == PROVISION_SUCCESS && restartAt_ && static_cast<int32_t>(millis() - restartAt_) >= 0) {
    ESP.restart();
  }
}

void ProvisioningPortal::finishScan(int16_t count) {
  scanInProgress_ = false;
  networkCount_ = 0;
  for (int16_t i = 0; i < count && networkCount_ < MAX_NETWORKS; ++i) {
    String name = WiFi.SSID(i);
    if (!name.length()) continue;
    int existing = -1;
    for (uint8_t j = 0; j < networkCount_; ++j) if (networks_[j].ssid == name) existing = j;
    if (existing >= 0) {
      if (WiFi.RSSI(i) > networks_[existing].rssi) networks_[existing].rssi = WiFi.RSSI(i);
      continue;
    }
    networks_[networkCount_++] = {name, WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN};
  }
  for (uint8_t i = 0; i < networkCount_; ++i) {
    for (uint8_t j = i + 1; j < networkCount_; ++j) {
      if (networks_[j].rssi > networks_[i].rssi) { ProvisionNetwork item = networks_[i]; networks_[i] = networks_[j]; networks_[j] = item; }
    }
  }
  WiFi.scanDelete();
  if (isPortal()) {
    message_ = "Wi-Fi 列表已更新";
    renderRequested_ = true;
    return;
  }
  if (ssid_.length()) beginStationConnection();
  else startPortal();
}

void ProvisioningPortal::beginStationConnection() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid_.c_str(), password_.c_str());
  setState(PROVISION_CONNECTING, "正在连接 " + ssid_);
}

void ProvisioningPortal::startPortal() {
  preferences_.putBool("portal", true);
  // Reinitialize the AP/DHCP side so a phone reconnect receives a fresh lease.
  dns_.stop();
  server_.stop();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(false, false);
  WiFi.setAutoReconnect(false);
  // Keep the AP alive while the STA interface performs background scans.
  // Auto-reconnect remains disabled so the old work network cannot compete
  // with the provisioning hotspot.
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  IPAddress apIp(192, 168, 4, 1);
  IPAddress apMask(255, 255, 255, 0);
  WiFi.softAPConfig(apIp, apIp, apMask);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
  apSsid_ = "SmartInspect-" + String(suffix);
  WiFi.softAP(apSsid_.c_str(), nullptr, 1, false, 4);
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
  server_.on("/api/rescan", HTTP_POST, [this]() { requestScan(); server_.send(202, "application/json", "{\"ok\":true}"); });
  server_.on("/api/provision", HTTP_POST, [this]() { handleProvisionPost(); });
  // Return the portal itself for OS connectivity probes. A redirect can be
  // interpreted as a successful internet check, suppressing the captive UI.
  server_.on("/generate_204", HTTP_GET, [this]() { sendPortalPage(); });
  server_.on("/hotspot-detect.html", HTTP_GET, [this]() { sendPortalPage(); });
  server_.on("/ncsi.txt", HTTP_GET, [this]() { sendPortalPage(); });
  server_.on("/connecttest.txt", HTTP_GET, [this]() { sendPortalPage(); });
  server_.on("/fwlink", HTTP_GET, [this]() { sendPortalPage(); });
  server_.onNotFound([this]() { sendRedirect(); });
}

void ProvisioningPortal::handleProvisionPost() {
  JsonDocument doc;
  if (deserializeJson(doc, server_.arg("plain"))) { server_.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"请求格式无效\"}"); return; }
  String newSsid = doc["ssid"] | "";
  String newPassword = doc["password"] | "";
  String newBackend = doc["backend_url"] | backendUrl_;
  newSsid.trim(); newBackend.trim();
  while (newBackend.endsWith("/")) newBackend.remove(newBackend.length() - 1);
  if (!newSsid.length() || newSsid.length() > 32) { server_.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"Wi-Fi 名称长度无效\"}"); return; }
  if (newPassword.length() && (newPassword.length() < 8 || newPassword.length() > 63)) { server_.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"Wi-Fi 密码长度无效\"}"); return; }
  if (!validBackend(newBackend)) { server_.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"后端地址必须以 http:// 开头\"}"); return; }
  pendingSsid_ = newSsid;
  pendingPassword_ = newPassword;
  pendingBackend_ = newBackend;
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(false, false);
  WiFi.begin(pendingSsid_.c_str(), pendingPassword_.c_str());
  setState(PROVISION_VERIFYING, "正在验证 Wi-Fi");
  server_.send(202, "application/json", "{\"ok\":true}");
}

void ProvisioningPortal::verifyProvisioningConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    ssid_ = pendingSsid_;
    password_ = pendingPassword_;
    backendUrl_ = pendingBackend_;
    backendHealthy_ = false;
    HTTPClient http;
    http.setConnectTimeout(1200);
    http.setTimeout(1800);
    if (http.begin(backendUrl_ + "/health")) { backendHealthy_ = http.GET() == HTTP_CODE_OK; http.end(); }
    saveProvisioning();
    setState(PROVISION_SUCCESS, backendHealthy_ ? "配置成功，正在重启" : "Wi-Fi 已连接，后端暂不可达");
    restartAt_ = millis() + RESTART_DELAY_MS;
  } else if (millis() - stateSince_ >= CONNECT_TIMEOUT_MS) {
    WiFi.disconnect(false, false);
    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_AP_STA);
    setState(PROVISION_FAILED, "连接失败，请返回网页修改配置");
  }
}

void ProvisioningPortal::saveProvisioning() {
  preferences_.putString("ssid", ssid_);
  preferences_.putString("password", password_);
  preferences_.putString("backend", backendUrl_);
  preferences_.putUChar("version", 1);
  preferences_.putBool("portal", false);
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
    case PROVISION_SCANNING: return "scanning";
    case PROVISION_CONNECTING: return "connecting";
    case PROVISION_NORMAL: return "normal";
    case PROVISION_AP: return "ap";
    case PROVISION_VERIFYING: return "verifying";
    case PROVISION_FAILED: return "failed";
    case PROVISION_SUCCESS: return "success";
  }
  return "unknown";
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

String ProvisioningPortal::html() const {
  return R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>智巡精灵网络配置</title>
<style>body{font-family:system-ui,sans-serif;background:#101417;color:#f4f7f8;margin:0;padding:20px}main{max-width:520px;margin:auto}h1{font-size:24px}label{display:block;margin:14px 0 6px}input,button{box-sizing:border-box;width:100%;padding:12px;border-radius:8px;border:1px solid #52616b;font-size:16px}input{background:#1b2328;color:#fff}button{background:#168d91;color:#fff;border:0;margin-top:16px}.secondary{background:#34434b}.network{display:flex;justify-content:space-between;padding:11px 0;border-bottom:1px solid #34434b;cursor:pointer}.muted{color:#9eabb2}.status{margin:16px 0;padding:12px;background:#1b2328;border-radius:8px}</style></head>
<body><main><h1>智巡精灵 v1.0</h1><p class="muted">配置设备要连接的工作网络</p><div id="status" class="status">正在读取状态...</div><button class="secondary" id="scan">刷新 Wi-Fi 列表</button><div id="nets"></div><label>Wi-Fi 名称</label><input id="ssid" maxlength="32" autocomplete="off"><label>Wi-Fi 密码（开放网络留空）</label><input id="password" type="password" maxlength="63" autocomplete="new-password"><label>后端地址</label><input id="backend" maxlength="160"><button id="save">验证并保存</button><p id="result" class="muted"></p></main>
<script>const $=id=>document.getElementById(id);async function getStatus(){const d=await(await fetch('/api/status')).json();$('status').textContent=d.message+(d.backend_healthy?'（后端正常）':'');if(d.state==='verifying')setTimeout(getStatus,1000)}async function load(){const cfg=await(await fetch('/api/config')).json();$('backend').value=cfg.backend_url;const d=await(await fetch('/api/networks')).json();const root=$('nets');root.textContent='';d.networks.forEach(n=>{const row=document.createElement('div');row.className='network';const name=document.createElement('span');name.textContent=n.ssid;const meta=document.createElement('span');meta.textContent=n.rssi+' dBm '+(n.secured?'锁':'开放');row.append(name,meta);row.onclick=()=>{$('ssid').value=n.ssid};root.append(row)});getStatus()}$('scan').onclick=async()=>{await fetch('/api/rescan',{method:'POST'});$('result').textContent='正在扫描...';setTimeout(load,1500)};$('save').onclick=async()=>{const body={ssid:$('ssid').value,password:$('password').value,backend_url:$('backend').value};const r=await fetch('/api/provision',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});const d=await r.json();$('result').textContent=d.ok?'正在验证 Wi-Fi，请保持热点连接...':d.error;getStatus()};load()</script></body></html>)HTML";
}

void ProvisioningPortal::sendPortalPage() {
  server_.sendHeader("Cache-Control", "no-store");
  server_.sendHeader("Connection", "close");
  server_.send(200, "text/html; charset=utf-8", html());
}
