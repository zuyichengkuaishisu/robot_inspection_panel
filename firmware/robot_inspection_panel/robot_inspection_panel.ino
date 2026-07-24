#define LGFX_USE_V1

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <LovyanGFX.hpp>
#include <qrcode.h>

#include "CST816D.h"
#include "ProvisioningPortal.h"
#include "config.h"

static constexpr uint16_t SCREEN_W = 240;
static constexpr uint16_t SCREEN_H = 240;
static constexpr uint8_t MAX_POINTS = 20;
static constexpr uint8_t MAX_INSPECTIONS = 4;
static constexpr unsigned long POLL_INTERVAL_MS = 1000;
static constexpr unsigned long ROBOT_STATUS_STALE_MS = 3000;
static constexpr int16_t POINT_VIEW_X = 40;
static constexpr int16_t POINT_VIEW_Y = 70;
static constexpr int16_t POINT_VIEW_W = 160;
static constexpr int16_t POINT_VIEW_H = 170;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 panel_;
  lgfx::Bus_SPI bus_;
 public:
  LGFX() {
    auto bus_cfg = bus_.config();
    bus_cfg.spi_host = SPI2_HOST; bus_cfg.spi_mode = 0;
    bus_cfg.freq_write = 80000000; bus_cfg.freq_read = 20000000;
    bus_cfg.spi_3wire = true; bus_cfg.use_lock = true; bus_cfg.dma_channel = SPI_DMA_CH_AUTO;
    bus_cfg.pin_sclk = 6; bus_cfg.pin_mosi = 7; bus_cfg.pin_miso = -1; bus_cfg.pin_dc = 2;
    bus_.config(bus_cfg); panel_.setBus(&bus_);
    auto panel_cfg = panel_.config();
    panel_cfg.pin_cs = 10; panel_cfg.pin_rst = -1; panel_cfg.pin_busy = -1;
    panel_cfg.memory_width = SCREEN_W; panel_cfg.memory_height = SCREEN_H;
    panel_cfg.panel_width = SCREEN_W; panel_cfg.panel_height = SCREEN_H;
    panel_cfg.offset_x = 0; panel_cfg.offset_y = 0; panel_cfg.offset_rotation = 0;
    panel_cfg.dummy_read_pixel = 8; panel_cfg.dummy_read_bits = 1;
    panel_cfg.readable = false; panel_cfg.invert = true; panel_cfg.rgb_order = false;
    panel_cfg.dlen_16bit = false; panel_cfg.bus_shared = false;
    panel_.config(panel_cfg); setPanel(&panel_);
  }
};

struct Option { char id[32]; char name[48]; };
enum Page {
  HOME,
  OVERRIDE_CONFIRM,
  POINT_LIST,
  CONFIRM_NAVIGATION,
  WAIT_ARRIVAL,
  DELIVERY_LOAD,
  DELIVERY_CONFIRM,
  DELIVERY_STATUS,
  PICK_INSPECTION,
  INSPECTION_STATUS,
  ERROR_PAGE,
  NETWORK_SETUP,
};
enum Workflow { FLOW_NONE, FLOW_DELIVERY, FLOW_INSPECTION };
enum PointListMode { LIST_PICKUP, LIST_INSPECTION_POINT, LIST_DELIVERY_DESTINATION };
enum PendingAction {
  NO_ACTION,
  LOAD_POINTS,
  LOAD_ROBOT_STATUS,
  LOAD_INSPECTIONS,
  SEND_NAVIGATION,
  ACK_LOAD,
  SEND_DELIVERY,
  SEND_INSPECTION,
  ACK_UNLOAD,
};

LGFX tft;
LGFX_Sprite point_list_canvas(&tft);
CST816D touch(4, 5, 1, 0);
ProvisioningPortal provisioning;
static Page page = HOME;
static Workflow workflow = FLOW_NONE;
static Workflow requested_workflow = FLOW_NONE;
static PointListMode point_list_mode = LIST_INSPECTION_POINT;
static Option points[MAX_POINTS], inspections[MAX_INSPECTIONS];
static uint8_t point_count = 0, inspection_count = 0;
static int selected_point = -1, pickup_point = -1, destination_point = -1, selected_inspection = -1;
static String task_id, pickup_task_id, task_status, task_message, error_message, replacement_task_id;
static int unload_remaining_seconds = 0;
static unsigned long last_poll = 0;
static unsigned long last_touch_diagnostic = 0;
static unsigned long last_home_refresh = 0;
static unsigned long last_status_success = 0;
static unsigned long completion_since = 0;
static int previous_touch_count = -2;
static bool raw_touch_pressed = false;
static unsigned long last_raw_tap_ms = 0;
static PendingAction pending_action = NO_ACTION;
static unsigned long pending_action_since = 0;
static int16_t point_scroll = 0;
static int16_t point_scroll_anchor = 0;
static bool point_scroll_dragging = false;
static bool point_scroll_moved = false;
static bool point_wait_for_release = false;
static uint16_t point_scroll_start_x = 0;
static uint16_t point_scroll_start_y = 0;
static uint8_t point_scroll_gesture = 0;
static unsigned long point_release_since = 0;
static bool point_list_canvas_ready = false;
static bool robot_status_known = false;
static bool robot_status_stale_rendered = false;
static String robot_state, robot_phase, robot_message;
static String robot_current_id, robot_current_name, robot_target_id, robot_target_name;
static String robot_active_id, robot_active_kind, robot_active_status, robot_active_purpose;
static String robot_active_source_id, robot_active_destination_id;
static String robot_status_signature;

#define LOGI(format, ...) do { \
  if (Serial && Serial.availableForWrite() >= 128) { \
    Serial.printf("[panel][%8lu] " format "\n", millis(), ##__VA_ARGS__); \
  } \
} while (0)

static void showPage(Page next);
static void renderDirectPage();
static void renderPointListViewport();
static void handleRawTap(uint16_t x, uint16_t y);
static void scheduleAction(PendingAction action);

static String endpoint(const char *path) { return provisioning.backendUrl() + path; }

static const char *pageName(Page value) {
  switch (value) {
    case HOME: return "HOME";
    case OVERRIDE_CONFIRM: return "OVERRIDE_CONFIRM";
    case POINT_LIST: return "POINT_LIST";
    case CONFIRM_NAVIGATION: return "CONFIRM_NAVIGATION";
    case WAIT_ARRIVAL: return "WAIT_ARRIVAL";
    case DELIVERY_LOAD: return "DELIVERY_LOAD";
    case DELIVERY_CONFIRM: return "DELIVERY_CONFIRM";
    case DELIVERY_STATUS: return "DELIVERY_STATUS";
    case PICK_INSPECTION: return "PICK_INSPECTION";
    case INSPECTION_STATUS: return "INSPECTION_STATUS";
    case ERROR_PAGE: return "ERROR";
    case NETWORK_SETUP: return "NETWORK_SETUP";
  }
  return "UNKNOWN";
}

static void addAuth(HTTPClient &http) {
  if (strlen(API_TOKEN)) http.addHeader("X-API-Token", API_TOKEN);
}

static bool getJson(const String &url, JsonDocument &doc) {
  HTTPClient http; http.setConnectTimeout(4000); http.setTimeout(5000);
  LOGI("HTTP GET %s", url.c_str());
  if (!http.begin(url)) { error_message = "后端地址无效"; return false; }
  addAuth(http);
  int code = http.GET();
  String body = http.getString(); http.end();
  LOGI("GET response code=%d bytes=%u", code, body.length());
  if (code != HTTP_CODE_OK) { error_message = code > 0 ? "后端错误 " + String(code) : "无法连接后端"; return false; }
  if (deserializeJson(doc, body)) { error_message = "后端响应无效"; return false; }
  return true;
}

static bool postJson(const String &path, JsonDocument &request, JsonDocument &response) {
  HTTPClient http; http.setConnectTimeout(4000); http.setTimeout(5000);
  String url = endpoint(path.c_str());
  LOGI("HTTP POST %s", url.c_str());
  if (!http.begin(url)) { error_message = "后端地址无效"; return false; }
  http.addHeader("Content-Type", "application/json"); addAuth(http);
  String body; serializeJson(request, body);
  int code = http.POST(body); String reply = http.getString(); http.end();
  LOGI("POST response code=%d request_bytes=%u response_bytes=%u", code, body.length(), reply.length());
  if (code != HTTP_CODE_OK && code != HTTP_CODE_CREATED) {
    error_message = code == HTTP_CODE_CONFLICT ? "机器狗任务状态已变化" : (code > 0 ? "请求失败 " + String(code) : "无法连接后端");
    return false;
  }
  if (deserializeJson(response, reply)) { error_message = "后端响应无效"; return false; }
  return true;
}

static String eventId(const char *prefix) {
  return String(PANEL_DEVICE_ID) + "-" + prefix + "-" + String(millis()) + "-" + String(esp_random(), HEX);
}

static int findPoint(const String &id) {
  for (uint8_t i = 0; i < point_count; ++i) if (id == points[i].id) return i;
  return -1;
}

static bool fetchPoints() {
  JsonDocument doc;
  if (!getJson(endpoint("/api/panels/") + PANEL_DEVICE_ID + "/config", doc)) return false;
  point_count = 0;
  for (JsonObject item : doc["points"].as<JsonArray>()) {
    if (point_count == MAX_POINTS) break;
    strlcpy(points[point_count].id, item["id"] | "", sizeof(points[0].id));
    strlcpy(points[point_count].name, item["name"] | "未命名点位", sizeof(points[0].name));
    ++point_count;
  }
  if (!point_count) { error_message = "暂无可用点位"; return false; }
  LOGI("Loaded %u point(s)", point_count);
  return true;
}

static bool fetchInspections() {
  if (selected_point < 0) { error_message = "巡检点无效"; return false; }
  JsonDocument doc;
  if (!getJson(endpoint("/api/points/") + points[selected_point].id + "/inspections", doc)) return false;
  inspection_count = 0;
  for (JsonObject item : doc["inspections"].as<JsonArray>()) {
    if (inspection_count == MAX_INSPECTIONS) break;
    strlcpy(inspections[inspection_count].id, item["id"] | "", sizeof(inspections[0].id));
    strlcpy(inspections[inspection_count].name, item["name"] | "巡检项目", sizeof(inspections[0].name));
    ++inspection_count;
  }
  if (!inspection_count) { error_message = "暂无巡检项目"; return false; }
  return true;
}

static String makeRobotSignature() {
  return robot_state + "|" + robot_phase + "|" + robot_current_id + "|" + robot_target_id + "|" + robot_active_id + "|" + robot_message;
}

static bool fetchRobotStatus() {
  JsonDocument doc;
  if (!getJson(endpoint("/api/robot/status"), doc)) return false;
  robot_state = doc["state"] | "";
  robot_phase = doc["phase"] | "";
  robot_message = doc["message"] | "";
  robot_current_id = doc["current_point"]["id"] | "";
  robot_current_name = doc["current_point"]["name"] | "位置未知";
  robot_target_id = doc["target_point"]["id"] | "";
  robot_target_name = doc["target_point"]["name"] | "";
  JsonObject active = doc["active_task"].as<JsonObject>();
  if (active) {
    robot_active_id = active["id"] | "";
    robot_active_kind = active["kind"] | "";
    robot_active_status = active["status"] | "";
    robot_active_purpose = active["purpose"] | "";
    robot_active_source_id = active["source_point_id"] | "";
    robot_active_destination_id = active["destination_point_id"] | "";
  } else {
    robot_active_id = robot_active_kind = robot_active_status = robot_active_purpose = "";
    robot_active_source_id = robot_active_destination_id = "";
  }
  unload_remaining_seconds = doc["unload_remaining_seconds"] | 0;
  String next_signature = makeRobotSignature();
  bool changed = next_signature != robot_status_signature;
  robot_status_signature = next_signature;
  robot_status_known = true;
  robot_status_stale_rendered = false;
  last_status_success = millis();
  if (changed && page == HOME) renderDirectPage();
  return true;
}

static bool robotStatusFresh() {
  return robot_status_known && millis() - last_status_success <= ROBOT_STATUS_STALE_MS;
}

static void applyTask(JsonDocument &doc) {
  task_id = doc["id"] | "";
  task_status = doc["status"] | "queued";
  task_message = doc["message"] | "";
  unload_remaining_seconds = doc["unload_remaining_seconds"] | 0;
  LOGI("Task id=%s status=%s message=%s", task_id.c_str(), task_status.c_str(), task_message.c_str());
}

static bool createNavigation() {
  if (selected_point < 0) return false;
  JsonDocument request, response;
  request["event_id"] = eventId("navigate");
  request["device_id"] = PANEL_DEVICE_ID;
  request["site_id"] = PANEL_SITE_ID;
  request["point_id"] = points[selected_point].id;
  request["purpose"] = workflow == FLOW_DELIVERY ? "delivery_pickup" : "inspection";
  if (replacement_task_id.length()) request["replace_task_id"] = replacement_task_id;
  if (!postJson("/api/navigation-tasks", request, response)) return false;
  applyTask(response);
  replacement_task_id = "";
  if (workflow == FLOW_DELIVERY) {
    pickup_point = selected_point;
    pickup_task_id = task_id;
  }
  return task_id.length() > 0;
}

static bool acknowledgeLoad() {
  JsonDocument request, response;
  request["event_id"] = eventId("loaded");
  request["device_id"] = PANEL_DEVICE_ID;
  if (!postJson("/api/navigation-tasks/" + pickup_task_id + "/load-complete", request, response)) return false;
  applyTask(response);
  pickup_task_id = task_id;
  return task_status == "awaiting_destination";
}

static bool createDelivery() {
  if (destination_point < 0 || pickup_task_id.isEmpty()) return false;
  JsonDocument request, response;
  request["event_id"] = eventId("delivery");
  request["device_id"] = PANEL_DEVICE_ID;
  request["pickup_task_id"] = pickup_task_id;
  request["destination_point_id"] = points[destination_point].id;
  if (!postJson("/api/delivery-tasks", request, response)) return false;
  applyTask(response);
  return task_id.length() > 0;
}

static bool createInspection() {
  if (selected_inspection < 0) return false;
  JsonDocument request, response;
  request["event_id"] = eventId("inspect");
  request["device_id"] = PANEL_DEVICE_ID;
  request["arrival_task_id"] = task_id;
  request["inspection_id"] = inspections[selected_inspection].id;
  if (!postJson("/api/inspection-tasks", request, response)) return false;
  applyTask(response);
  return task_id.length() > 0;
}

static bool acknowledgeUnload() {
  JsonDocument request, response;
  request["event_id"] = eventId("unloaded");
  request["device_id"] = PANEL_DEVICE_ID;
  if (!postJson("/api/delivery-tasks/" + task_id + "/unload-complete", request, response)) return false;
  applyTask(response);
  return task_status == "completed";
}

static bool pollTask() {
  JsonDocument doc;
  if (!getJson(endpoint("/api/tasks/") + task_id, doc)) return false;
  String previous = task_status;
  task_status = doc["status"] | "failed";
  task_message = doc["message"] | "暂无状态";
  unload_remaining_seconds = doc["unload_remaining_seconds"] | 0;
  if (previous != task_status) LOGI("Task %s status %s -> %s", task_id.c_str(), previous.c_str(), task_status.c_str());
  return true;
}

static void clearLocalWorkflow() {
  workflow = FLOW_NONE;
  requested_workflow = FLOW_NONE;
  selected_point = pickup_point = destination_point = selected_inspection = -1;
  inspection_count = 0;
  task_id = pickup_task_id = replacement_task_id = "";
  task_status = task_message = "";
  completion_since = 0;
}

static void goHome() {
  clearLocalWorkflow();
  showPage(HOME);
}

static void openPointList(PointListMode mode) {
  point_list_mode = mode;
  point_scroll = 0;
  point_wait_for_release = true;
  showPage(POINT_LIST);
}

static void beginWorkflow(Workflow next) {
  if (!robotStatusFresh()) {
    error_message = "机器狗状态不可用";
    showPage(ERROR_PAGE);
    return;
  }
  requested_workflow = next;
  if (robot_state == "busy") {
    showPage(OVERRIDE_CONFIRM);
    return;
  }
  workflow = next;
  replacement_task_id = "";
  openPointList(next == FLOW_DELIVERY ? LIST_PICKUP : LIST_INSPECTION_POINT);
}

static void confirmOverride() {
  if (!robotStatusFresh() || robot_active_id.isEmpty()) {
    error_message = "当前任务状态已变化";
    showPage(ERROR_PAGE);
    return;
  }
  workflow = requested_workflow;
  replacement_task_id = robot_active_id;
  openPointList(workflow == FLOW_DELIVERY ? LIST_PICKUP : LIST_INSPECTION_POINT);
}

static void resumeActiveTask() {
  if (!robotStatusFresh() || robot_state != "busy" || robot_active_id.isEmpty()) return;
  task_id = robot_active_id;
  task_status = robot_active_status;
  task_message = robot_message;
  selected_point = findPoint(robot_target_id);
  if (robot_phase == "going_to_pickup") {
    workflow = FLOW_DELIVERY;
    pickup_point = selected_point;
    pickup_task_id = task_id;
    showPage(WAIT_ARRIVAL);
  } else if (robot_phase == "awaiting_load") {
    workflow = FLOW_DELIVERY;
    pickup_point = selected_point;
    pickup_task_id = task_id;
    showPage(DELIVERY_LOAD);
  } else if (robot_phase == "awaiting_destination") {
    workflow = FLOW_DELIVERY;
    pickup_point = selected_point;
    pickup_task_id = task_id;
    openPointList(LIST_DELIVERY_DESTINATION);
  } else if (robot_phase == "going_to_inspection") {
    workflow = FLOW_INSPECTION;
    showPage(WAIT_ARRIVAL);
  } else if (robot_phase == "awaiting_inspection") {
    workflow = FLOW_INSPECTION;
    inspection_count = 0;
    showPage(PICK_INSPECTION);
  } else if (robot_phase == "inspecting") {
    workflow = FLOW_INSPECTION;
    showPage(INSPECTION_STATUS);
  } else if (robot_phase == "delivering" || robot_phase == "awaiting_unload") {
    workflow = FLOW_DELIVERY;
    pickup_point = findPoint(robot_active_source_id);
    destination_point = findPoint(robot_active_destination_id);
    showPage(DELIVERY_STATUS);
  }
}

static void confirmNavigation() {
  task_status = "sending";
  task_message = workflow == FLOW_DELIVERY ? "正在发送召唤任务" : "正在发送巡检导航";
  showPage(WAIT_ARRIVAL);
  scheduleAction(SEND_NAVIGATION);
}

static void selectInspection() {
  task_status = "sending";
  task_message = "正在启动巡检";
  showPage(INSPECTION_STATUS);
  scheduleAction(SEND_INSPECTION);
}

static uint8_t listItemCount() {
  if (point_list_mode != LIST_DELIVERY_DESTINATION || pickup_point < 0) return point_count;
  return point_count > 0 ? point_count - 1 : 0;
}

static int pointIndexForSlot(uint8_t slot) {
  if (point_list_mode != LIST_DELIVERY_DESTINATION || pickup_point < 0) return slot < point_count ? slot : -1;
  uint8_t seen = 0;
  for (uint8_t i = 0; i < point_count; ++i) {
    if (i == pickup_point) continue;
    if (seen++ == slot) return i;
  }
  return -1;
}

static int pointMaxScroll() {
  return max(0, (int)listItemCount() * 38 - (230 - 78));
}

static void removeLastUtf8Character(String &text) {
  int index = text.length() - 1;
  while (index > 0 && (static_cast<uint8_t>(text[index]) & 0xC0) == 0x80) --index;
  text.remove(index);
}

static int circularSafeWidth(int y) {
  const int radius = 116;
  const int distance = abs(y - SCREEN_H / 2);
  if (distance >= radius) return 0;
  return 2 * sqrt(radius * radius - distance * distance) - 8;
}

static String localizedStatus(const String &status) {
  if (status == "sending") return "发送中";
  if (status == "queued") return "排队中";
  if (status == "navigating") return "前往点位";
  if (status == "arrived") return "已到达";
  if (status == "awaiting_load") return "等待装货";
  if (status == "awaiting_destination") return "等待送货点";
  if (status == "delivering") return "送货中";
  if (status == "awaiting_unload") return "等待卸货";
  if (status == "running") return "巡检中";
  if (status == "completed") return "已完成";
  if (status == "cancelled") return "已取消";
  if (status == "failed") return "失败";
  return status;
}

static String robotPhaseLabel() {
  if (robot_phase == "going_to_pickup") return "任务中 · 前往取货点";
  if (robot_phase == "awaiting_load") return "任务中 · 等待装货";
  if (robot_phase == "awaiting_destination") return "任务中 · 等待送货点";
  if (robot_phase == "delivering") return "任务中 · 正在送货";
  if (robot_phase == "awaiting_unload") return "任务中 · 等待卸货";
  if (robot_phase == "going_to_inspection") return "任务中 · 前往巡检点";
  if (robot_phase == "awaiting_inspection") return "任务中 · 等待巡检";
  if (robot_phase == "inspecting") return "任务中 · 正在巡检";
  return "任务中";
}

static void directText(const String &text, int y, uint8_t size = 1, uint16_t color = TFT_WHITE) {
  String visible = text;
  uint8_t actual_size = size;
  tft.setFont(&fonts::efontCN_16);
  const int max_width = max(80, min(216, circularSafeWidth(y)));
  tft.setTextSize(actual_size);
  while (actual_size > 1 && tft.textWidth(visible) > max_width) {
    --actual_size;
    tft.setTextSize(actual_size);
  }
  if (tft.textWidth(visible) > max_width) {
    while (visible.length() > 3 && tft.textWidth(visible + "...") > max_width) removeLastUtf8Character(visible);
    visible += "...";
  }
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextDatum(lgfx::middle_center);
  tft.drawString(visible, SCREEN_W / 2, y);
}

static void directButton(const String &text, int top, uint16_t border = TFT_CYAN) {
  tft.fillRoundRect(40, top, 160, 34, 5, TFT_DARKGREY);
  tft.drawRoundRect(40, top, 160, 34, 5, border);
  directText(text, top + 17, 1, TFT_WHITE);
}

static void directSideButton(const String &text, int top, uint16_t border = TFT_CYAN) {
  const int left = 8;
  const int width = 32;
  tft.fillRoundRect(left, top, width, 34, 5, TFT_DARKGREY);
  tft.drawRoundRect(left, top, width, 34, 5, border);
  tft.setFont(&fonts::efontCN_16);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(lgfx::middle_center);
  tft.drawString(text, left + width / 2, top + 17);
}

static void renderUnloadCountdown() {
  int minutes = unload_remaining_seconds / 60;
  int seconds = unload_remaining_seconds % 60;
  tft.fillRect(28, 116, 184, 24, TFT_BLACK);
  directText("自动完成 " + String(minutes) + ":" + (seconds < 10 ? "0" : "") + String(seconds), 128, 0);
}

static void renderPointListViewport() {
  if (!point_list_canvas_ready) return;
  point_scroll = constrain(point_scroll, 0, pointMaxScroll());
  point_list_canvas.fillScreen(0);
  point_list_canvas.setFont(&fonts::efontCN_16);
  point_list_canvas.setTextSize(1);
  point_list_canvas.setTextDatum(lgfx::middle_center);
  point_list_canvas.setTextColor(3);
  for (uint8_t slot = 0; slot < listItemCount(); ++slot) {
    int point_index = pointIndexForSlot(slot);
    int top = 78 + slot * 38 - point_scroll - POINT_VIEW_Y;
    if (point_index < 0 || top >= POINT_VIEW_H || top + 34 <= 0) continue;
    point_list_canvas.fillRoundRect(0, top, 160, 34, 5, 1);
    point_list_canvas.drawRoundRect(0, top, 160, 34, 5, 2);
    String visible = points[point_index].name;
    bool truncated = point_list_canvas.textWidth(visible) > 148;
    while (visible.length() > 3 && point_list_canvas.textWidth(visible + "...") > 148) removeLastUtf8Character(visible);
    if (truncated) visible += "...";
    point_list_canvas.drawString(visible, POINT_VIEW_W / 2, top + 17);
  }
  point_list_canvas.pushSprite(POINT_VIEW_X, POINT_VIEW_Y);
}

static void renderProvisioningQr(esp_qrcode_handle_t qr) {
  const int qrSize = esp_qrcode_get_size(qr);
  const int scale = 3;
  const int quiet = 4;
  const int side = (qrSize + quiet * 2) * scale;
  const int left = (SCREEN_W - side) / 2;
  const int top = 50;
  tft.fillRect(left, top, side, side, TFT_WHITE);
  for (int y = 0; y < qrSize; ++y) for (int x = 0; x < qrSize; ++x) {
    if (esp_qrcode_get_module(qr, x, y)) tft.fillRect(left + (x + quiet) * scale, top + (y + quiet) * scale, scale, scale, TFT_BLACK);
  }
}

static void drawProvisioningQr() {
  String payload = "WIFI:T:nopass;S:" + provisioning.apSsid() + ";;";
  esp_qrcode_config_t config = ESP_QRCODE_CONFIG_DEFAULT();
  config.display_func = renderProvisioningQr;
  config.max_qrcode_version = 3;
  config.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
  esp_qrcode_generate(&config, payload.c_str());
}

static void renderDirectPage() {
  unsigned long started = millis();
  tft.fillScreen(TFT_BLACK);
  if (page == NETWORK_SETUP) {
    ProvisioningState state = provisioning.state();
    directText("网络配置", 31, 1, TFT_CYAN);
    if (state == PROVISION_AP || state == PROVISION_FAILED) {
      drawProvisioningQr();
      directText(provisioning.apSsid(), 180);
      directText("扫码加入热点", 198);
      directText("192.168.4.1", 216, 1, TFT_CYAN);
      if (state == PROVISION_FAILED) directText(provisioning.message(), 100, 1, TFT_RED);
    } else if (state == PROVISION_VERIFYING) {
      directText("正在连接 Wi-Fi", 86, 1, TFT_YELLOW);
      directText(provisioning.candidateSsid(), 115);
      directText("请保持手机连接热点", 145);
    } else if (state == PROVISION_CONNECTING) {
      directText("配置成功", 100, 1, TFT_GREEN);
      directText(provisioning.message(), 140);
      directText("即将重启...", 170, 1, TFT_YELLOW);
    }
  } else if (page == HOME) {
    directText(PANEL_SITE_NAME, 22, 1, TFT_CYAN);
    if (!robotStatusFresh()) {
      directText("机器狗状态未知", 50, 1, TFT_RED);
      directText(WiFi.status() == WL_CONNECTED ? "等待后端状态" : "Wi-Fi 未连接", 72, 0, TFT_YELLOW);
    } else if (robot_state == "idle") {
      directText("空闲 · " + robot_current_name, 50, 1, TFT_GREEN);
      directText("机器狗可接收任务", 72, 0, TFT_WHITE);
    } else {
      directText(robotPhaseLabel(), 50, 1, TFT_YELLOW);
      String position = "位置 " + robot_current_name;
      if (robot_target_name.length() && robot_target_id != robot_current_id) position += " → " + robot_target_name;
      directText(position, 72, 0, TFT_WHITE);
    }
    uint16_t business_border = robotStatusFresh() ? TFT_CYAN : TFT_DARKGREY;
    directButton("送货", 90, business_border);
    directButton("巡检", 132, business_border);
    directButton("配置网络", 174);
  } else if (page == OVERRIDE_CONFIRM) {
    directText("机器狗正在执行任务", 38, 1, TFT_YELLOW);
    directText(robotPhaseLabel(), 68);
    directText(robot_message, 100, 0, TFT_WHITE);
    directButton(requested_workflow == FLOW_DELIVERY ? "终止并新建送货" : "终止并新建巡检", 140, TFT_RED);
    directButton("返回", 180);
  } else if (page == POINT_LIST) {
    String title = point_list_mode == LIST_PICKUP ? "选择召唤点" : (point_list_mode == LIST_DELIVERY_DESTINATION ? "选择送货点" : "选择巡检点");
    directText(title, 30, 1, TFT_CYAN);
    directText("上下滑动查看更多", 56, 0, TFT_DARKGREY);
    directSideButton("返回", 116);
    if (point_list_canvas_ready) renderPointListViewport();
    else for (uint8_t slot = 0; slot < listItemCount(); ++slot) {
      int point_index = pointIndexForSlot(slot);
      int top = 78 + slot * 38 - point_scroll;
      if (point_index >= 0 && top >= 72 && top <= 234) directButton(points[point_index].name, top);
    }
  } else if (page == CONFIRM_NAVIGATION) {
    directText(workflow == FLOW_DELIVERY ? "确认召唤" : "确认巡检点", 40, 1, TFT_YELLOW);
    directText(workflow == FLOW_DELIVERY ? "机器狗前往取货点" : "机器狗前往巡检点", 76);
    directText(selected_point >= 0 ? points[selected_point].name : "点位未知", 104, 1, TFT_CYAN);
    if (replacement_task_id.length()) directText("确认后将终止当前任务", 126, 0, TFT_RED);
    directButton("确认", 144);
    directButton("返回", 184);
  } else if (page == WAIT_ARRIVAL) {
    directText(workflow == FLOW_DELIVERY ? "等待机器狗" : "前往巡检点", 40, 1, TFT_CYAN);
    directText(selected_point >= 0 ? points[selected_point].name : robot_target_name, 72);
    directText(localizedStatus(task_status), 108, 1, TFT_YELLOW);
    directText(task_message, 136, 0);
    directButton("返回首页", 180);
  } else if (page == DELIVERY_LOAD) {
    directText("机器狗已到达", 40, 1, TFT_GREEN);
    directText(pickup_point >= 0 ? points[pickup_point].name : robot_current_name, 76, 1, TFT_CYAN);
    directText("请完成装货", 108);
    directButton("装货完成", 140);
    directButton("返回首页", 180);
  } else if (page == DELIVERY_CONFIRM) {
    directText("确认送货", 38, 1, TFT_YELLOW);
    directText(pickup_point >= 0 ? points[pickup_point].name : "取货点", 72);
    directText("送往", 98);
    directText(destination_point >= 0 ? points[destination_point].name : "送货点", 122, 1, TFT_CYAN);
    directButton("开始送货", 148);
    directButton("返回选点", 186);
  } else if (page == DELIVERY_STATUS) {
    directText("送货状态", 38, 1, TFT_CYAN);
    directText(destination_point >= 0 ? points[destination_point].name : robot_target_name, 70);
    directText(localizedStatus(task_status), 104, 1, task_status == "completed" ? TFT_GREEN : TFT_YELLOW);
    if (task_status == "awaiting_unload") {
      renderUnloadCountdown();
      directButton("卸货完成", 146);
      directButton("返回首页", 184);
    } else {
      directText(task_message, 134, 0);
      directButton(task_status == "completed" ? "完成" : "返回首页", 180);
    }
  } else if (page == PICK_INSPECTION) {
    directText("机器狗已到达", 38, 1, TFT_GREEN);
    directText(selected_point >= 0 ? points[selected_point].name : robot_current_name, 66, 1, TFT_CYAN);
    if (inspection_count == 0) directText("正在加载巡检项目...", 112);
    else {
      directText("请选择巡检项目", 88);
      for (uint8_t i = 0; i < inspection_count; ++i) directButton(inspections[i].name, 96 + i * 36);
    }
    if (inspection_count <= 2) directButton("返回首页", 184);
  } else if (page == INSPECTION_STATUS) {
    directText("巡检状态", 40, 1, TFT_CYAN);
    directText(selected_point >= 0 ? points[selected_point].name : robot_current_name, 70);
    directText(localizedStatus(task_status), 108, 1, task_status == "completed" ? TFT_GREEN : TFT_YELLOW);
    directText(task_message, 136, 0);
    directButton(task_status == "completed" ? "完成" : "返回首页", 180);
  } else {
    directText("操作失败", 48, 1, TFT_RED);
    directText(error_message, 108);
    directButton("返回首页", 180);
  }
  LOGI("Direct render page=%s elapsed=%lu ms", pageName(page), millis() - started);
}

static void showPage(Page next) {
  LOGI("UI page %s -> %s", pageName(page), pageName(next));
  page = next;
  last_poll = millis();
  renderDirectPage();
  if (page == HOME && provisioning.isConnected() && WiFi.status() == WL_CONNECTED) {
    if (point_count == 0) scheduleAction(LOAD_POINTS);
    else scheduleAction(LOAD_ROBOT_STATUS);
  }
  if (page == PICK_INSPECTION && inspection_count == 0) scheduleAction(LOAD_INSPECTIONS);
}

static void scheduleAction(PendingAction action) {
  if (pending_action != NO_ACTION && action == LOAD_ROBOT_STATUS) return;
  pending_action = action;
  pending_action_since = millis();
  LOGI("Scheduled network action=%d", action);
}

static void runPendingAction() {
  PendingAction action = pending_action;
  pending_action = NO_ACTION;
  bool ok = false;
  if (action == LOAD_ROBOT_STATUS) {
    fetchRobotStatus();
    return;
  }
  if (action == LOAD_POINTS) {
    ok = fetchPoints();
    if (ok) showPage(page); else showPage(ERROR_PAGE);
    return;
  }
  if (action == LOAD_INSPECTIONS) {
    ok = fetchInspections();
    showPage(ok ? PICK_INSPECTION : ERROR_PAGE);
    return;
  }
  if (action == SEND_NAVIGATION) ok = createNavigation();
  else if (action == ACK_LOAD) ok = acknowledgeLoad();
  else if (action == SEND_DELIVERY) ok = createDelivery();
  else if (action == SEND_INSPECTION) ok = createInspection();
  else if (action == ACK_UNLOAD) ok = acknowledgeUnload();
  if (!ok) { showPage(ERROR_PAGE); return; }
  if (action == SEND_NAVIGATION) showPage(WAIT_ARRIVAL);
  else if (action == ACK_LOAD) openPointList(LIST_DELIVERY_DESTINATION);
  else if (action == SEND_DELIVERY) showPage(DELIVERY_STATUS);
  else if (action == SEND_INSPECTION) showPage(INSPECTION_STATUS);
  else if (action == ACK_UNLOAD) { completion_since = millis(); showPage(DELIVERY_STATUS); }
}

static void handleRawTap(uint16_t x, uint16_t y) {
  LOGI("Raw tap x=%u y=%u page=%s", x, y, pageName(page));
  if (page == HOME) {
    if (y >= 38 && y < 84 && robotStatusFresh() && robot_state == "busy") { resumeActiveTask(); return; }
    if (y >= 90 && y < 124) { beginWorkflow(FLOW_DELIVERY); return; }
    if (y >= 132 && y < 166) { beginWorkflow(FLOW_INSPECTION); return; }
    if (y >= 174 && y < 208) { provisioning.startPortal(); page = NETWORK_SETUP; renderDirectPage(); return; }
  } else if (page == OVERRIDE_CONFIRM) {
    if (y >= 140 && y < 174) { confirmOverride(); return; }
    if (y >= 180) { showPage(HOME); return; }
  } else if (page == POINT_LIST) {
    if (x < 40 && y >= 116 && y < 150) {
      goHome();
      return;
    }
    if (y >= 78 && y <= 230) {
      for (uint8_t slot = 0; slot < listItemCount(); ++slot) {
        int top = 78 + slot * 38 - point_scroll;
        if (y >= top && y < top + 34) {
          int point_index = pointIndexForSlot(slot);
          if (point_index < 0) return;
          if (point_list_mode == LIST_DELIVERY_DESTINATION) {
            destination_point = point_index;
            showPage(DELIVERY_CONFIRM);
          } else {
            selected_point = point_index;
            if (workflow == FLOW_DELIVERY) pickup_point = point_index;
            showPage(CONFIRM_NAVIGATION);
          }
          return;
        }
      }
    }
  } else if (page == CONFIRM_NAVIGATION) {
    if (y >= 144 && y < 178) { confirmNavigation(); return; }
    if (y >= 184) { openPointList(workflow == FLOW_DELIVERY ? LIST_PICKUP : LIST_INSPECTION_POINT); return; }
  } else if (page == WAIT_ARRIVAL) {
    if (y >= 180) { goHome(); return; }
  } else if (page == DELIVERY_LOAD) {
    if (y >= 140 && y < 174) { task_status = "sending"; task_message = "正在确认装货"; scheduleAction(ACK_LOAD); renderDirectPage(); return; }
    if (y >= 180) { goHome(); return; }
  } else if (page == DELIVERY_CONFIRM) {
    if (y >= 148 && y < 182) { task_status = "sending"; task_message = "正在创建送货任务"; showPage(DELIVERY_STATUS); scheduleAction(SEND_DELIVERY); return; }
    if (y >= 186) { openPointList(LIST_DELIVERY_DESTINATION); return; }
  } else if (page == DELIVERY_STATUS) {
    if (task_status == "awaiting_unload" && y >= 146 && y < 180) { scheduleAction(ACK_UNLOAD); return; }
    if (y >= 180) { goHome(); return; }
  } else if (page == PICK_INSPECTION) {
    for (uint8_t i = 0; i < inspection_count; ++i) {
      int top = 96 + i * 36;
      if (y >= top && y < top + 34) { selected_inspection = i; selectInspection(); return; }
    }
    if (inspection_count <= 2 && y >= 184) { goHome(); return; }
  } else if (page == INSPECTION_STATUS) {
    if (y >= 180) { goHome(); return; }
  } else if (page == ERROR_PAGE && y >= 180) {
    goHome();
  }
}

void setup() {
  pinMode(3, OUTPUT); digitalWrite(3, LOW);
  Serial.begin(115200); delay(300);
  LOGI("Boot: Robot inspection panel");
  LOGI("Config: device=%s site=%s backend=%s", PANEL_DEVICE_ID, PANEL_SITE_ID, BACKEND_URL);
  tft.init();
  tft.fillScreen(TFT_BLACK);
  point_list_canvas.setColorDepth(lgfx::palette_2bit);
  point_list_canvas_ready = point_list_canvas.createSprite(POINT_VIEW_W, POINT_VIEW_H) != nullptr;
  if (point_list_canvas_ready) {
    point_list_canvas.setPaletteColor(0, 0x000000U);
    point_list_canvas.setPaletteColor(1, 0x555555U);
    point_list_canvas.setPaletteColor(2, 0x00FFFFU);
    point_list_canvas.setPaletteColor(3, 0xFFFFFFU);
  }
  tft.startWrite();
  touch.begin();
  pinMode(0, INPUT);
  LOGI("Display initialized; CST816D I2C probe at 0x15: %s", touch.isConnected() ? "found" : "NOT FOUND");
  provisioning.begin(BACKEND_URL, PANEL_DEVICE_ID);
  showPage(HOME);
  digitalWrite(3, HIGH);
}

void loop() {
  uint16_t raw_x = 0, raw_y = 0;
  uint8_t raw_gesture = 0;
  bool raw_touched = touch.getTouch(&raw_x, &raw_y, &raw_gesture);
  Page page_at_touch_start = page;
  if (raw_touched && !raw_touch_pressed && millis() - last_raw_tap_ms >= 350 && page_at_touch_start != POINT_LIST) {
    last_raw_tap_ms = millis();
    handleRawTap(raw_x, raw_y);
  }
  raw_touch_pressed = raw_touched;

  if (page == POINT_LIST) {
    if (point_wait_for_release) {
      if (!raw_touched) point_wait_for_release = false;
      point_scroll_dragging = false;
      point_release_since = 0;
    } else if (raw_touched) {
      point_release_since = 0;
      if (!point_scroll_dragging) {
        point_scroll_dragging = true;
        point_scroll_moved = false;
        point_scroll_anchor = point_scroll;
        point_scroll_start_x = raw_x;
        point_scroll_start_y = raw_y;
        point_scroll_gesture = raw_gesture;
      } else {
        if (raw_gesture) point_scroll_gesture = raw_gesture;
        int16_t delta = (int16_t)point_scroll_start_y - (int16_t)raw_y;
        if (abs(delta) >= 8) {
          point_scroll_moved = true;
          int16_t next = constrain(point_scroll_anchor + delta, 0, pointMaxScroll());
          if (next != point_scroll) { point_scroll = next; point_list_canvas_ready ? renderPointListViewport() : renderDirectPage(); }
        }
      }
    } else if (point_scroll_dragging) {
      if (!point_release_since) point_release_since = millis();
      else if (millis() - point_release_since >= 40) {
        if (!point_scroll_moved && (point_scroll_gesture == 1 || point_scroll_gesture == 2)) {
          point_scroll += point_scroll_gesture == 1 ? 114 : -114;
          point_scroll = constrain(point_scroll, 0, pointMaxScroll());
          point_scroll_moved = true;
        }
        bool was_tap = !point_scroll_moved;
        uint16_t tap_x = point_scroll_start_x;
        uint16_t tap_y = point_scroll_start_y;
        point_scroll_dragging = false;
        point_release_since = 0;
        point_list_canvas_ready ? renderPointListViewport() : renderDirectPage();
        if (was_tap && millis() - last_raw_tap_ms >= 350) {
          last_raw_tap_ms = millis();
          handleRawTap(tap_x, tap_y);
        }
      }
    }
  } else {
    point_scroll_dragging = false;
    point_release_since = 0;
  }

  provisioning.loop();
  if (provisioning.needsRender()) {
    if (provisioning.isPortal()) page = NETWORK_SETUP;
    else if (page == NETWORK_SETUP) { page = HOME; point_count = 0; robot_status_known = false; }
    renderDirectPage();
    provisioning.clearRenderRequest();
    if (provisioning.isConnected() && WiFi.status() == WL_CONNECTED) scheduleAction(LOAD_POINTS);
  }
  if (pending_action != NO_ACTION && millis() - pending_action_since >= 50) runPendingAction();

  if (page == HOME && provisioning.isConnected() && WiFi.status() == WL_CONNECTED && pending_action == NO_ACTION && millis() - last_home_refresh >= POLL_INTERVAL_MS) {
    last_home_refresh = millis();
    scheduleAction(point_count == 0 ? LOAD_POINTS : LOAD_ROBOT_STATUS);
  }
  if (page == HOME && robot_status_known && !robotStatusFresh() && !robot_status_stale_rendered) {
    robot_status_stale_rendered = true;
    renderDirectPage();
  }

  if ((page == WAIT_ARRIVAL || page == DELIVERY_STATUS || page == INSPECTION_STATUS) && task_id.length() && pending_action == NO_ACTION && millis() - last_poll >= POLL_INTERVAL_MS) {
    last_poll = millis();
    String previous = task_status;
    if (!pollTask()) showPage(ERROR_PAGE);
    else if (page == WAIT_ARRIVAL && workflow == FLOW_DELIVERY && task_status == "awaiting_load") showPage(DELIVERY_LOAD);
    else if (page == WAIT_ARRIVAL && workflow == FLOW_INSPECTION && task_status == "arrived") { inspection_count = 0; showPage(PICK_INSPECTION); }
    else if (task_status == "cancelled" || task_status == "failed") showPage(page);
    else if (task_status == "completed") {
      if (!completion_since) completion_since = millis();
      showPage(page);
    } else if (previous != task_status) showPage(page);
    else if (page == DELIVERY_STATUS && task_status == "awaiting_unload") renderUnloadCountdown();
  }
  if (completion_since && millis() - completion_since >= 1500 && (page == DELIVERY_STATUS || page == INSPECTION_STATUS)) goHome();

  if (millis() - last_touch_diagnostic >= 250) {
    last_touch_diagnostic = millis();
    int count = touch.touchCount();
    if (count != previous_touch_count) {
      LOGI("CST816D raw touch count=%d", count);
      previous_touch_count = count;
    }
  }
  delay(5);
}
