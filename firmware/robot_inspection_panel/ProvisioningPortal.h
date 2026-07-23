#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

enum ProvisioningState { PROVISION_SCANNING, PROVISION_CONNECTING, PROVISION_NORMAL, PROVISION_AP, PROVISION_VERIFYING, PROVISION_FAILED, PROVISION_SUCCESS };

struct ProvisionNetwork {
  String ssid;
  int32_t rssi;
  bool secured;
};

class ProvisioningPortal {
 public:
  void begin(const char *fallbackSsid, const char *fallbackPassword, const char *fallbackBackend, const char *deviceId);
  void loop();
  void startPortal();
  void requestScan();

  ProvisioningState state() const { return state_; }
  bool isPortal() const { return state_ == PROVISION_AP || state_ == PROVISION_VERIFYING || state_ == PROVISION_FAILED || state_ == PROVISION_SUCCESS; }
  bool isNormal() const { return state_ == PROVISION_NORMAL; }
  bool needsRender() const { return renderRequested_; }
  void clearRenderRequest() { renderRequested_ = false; }
  const String &ssid() const { return ssid_; }
  const String &backendUrl() const { return backendUrl_; }
  const String &apSsid() const { return apSsid_; }
  const String &message() const { return message_; }
  const String &candidateSsid() const { return pendingSsid_; }
  bool backendHealthy() const { return backendHealthy_; }

 private:
  static constexpr uint8_t MAX_NETWORKS = 16;
  static constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;
  static constexpr uint32_t RESTART_DELAY_MS = 2200;

  Preferences preferences_;
  WebServer server_{80};
  DNSServer dns_;
  ProvisioningState state_ = PROVISION_SCANNING;
  ProvisionNetwork networks_[MAX_NETWORKS];
  uint8_t networkCount_ = 0;
  String ssid_, password_, backendUrl_, deviceId_, apSsid_;
  String pendingSsid_, pendingPassword_, pendingBackend_;
  String message_ = "正在扫描 Wi-Fi";
  uint32_t stateSince_ = 0;
  uint32_t restartAt_ = 0;
  bool scanInProgress_ = false;
  bool routesReady_ = false;
  bool renderRequested_ = true;
  bool backendHealthy_ = false;

  void setState(ProvisioningState next, const String &message);
  void finishScan(int16_t count);
  void beginStationConnection();
  void verifyProvisioningConnection();
  void saveProvisioning();
  void setupRoutes();
  void sendPortalPage();
  void sendJson(int code, JsonDocument &doc);
  void sendRedirect();
  void handleProvisionPost();
  void sendNetworks();
  void sendStatus();
  bool validBackend(const String &value) const;
  String stateName() const;
  String html() const;
};
