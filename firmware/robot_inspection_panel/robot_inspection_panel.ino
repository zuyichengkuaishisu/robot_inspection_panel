#define LGFX_USE_V1

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <lvgl.h>
#include <LovyanGFX.hpp>
#include <qrcode.h>

#include "CST816D.h"
#include "ProvisioningPortal.h"
#include "config.h"

static constexpr uint16_t SCREEN_W = 240;
static constexpr uint16_t SCREEN_H = 240;
static constexpr uint8_t DRAW_ROWS = 24;
static constexpr uint8_t MAX_POINTS = 20;
static constexpr uint8_t MAX_INSPECTIONS = 4;
static constexpr unsigned long POLL_INTERVAL_MS = 1000;
static constexpr int16_t POINT_VIEW_X = 32;
static constexpr int16_t POINT_VIEW_Y = 70;
static constexpr int16_t POINT_VIEW_W = 176;
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
enum Page { HOME, POINT_LIST, CONFIRM_CALL, WAIT_ARRIVAL, PICK_INSPECTION, INSPECTION_STATUS, ERROR_PAGE, NETWORK_SETUP };
enum PendingAction { NO_ACTION, LOAD_POINTS, LOAD_INSPECTIONS, SEND_NAVIGATION, SEND_INSPECTION };

LGFX tft;
LGFX_Sprite point_list_canvas(&tft);
CST816D touch(4, 5, 1, 0);
ProvisioningPortal provisioning;
static lv_color_t draw_a[SCREEN_W * DRAW_ROWS];
static lv_color_t draw_b[SCREEN_W * DRAW_ROWS];
static lv_display_t *display;
static lv_obj_t *screen;
static lv_indev_t *touch_input;
static Page page = HOME;
static Option points[MAX_POINTS], inspections[MAX_INSPECTIONS];
static uint8_t point_count = 0, inspection_count = 0;
static int selected_point = -1, selected_inspection = -1;
static String task_id, task_status, task_message, error_message;
static unsigned long last_poll = 0;
static unsigned long last_touch_diagnostic = 0;
static unsigned long last_home_refresh = 0;
static int previous_touch_count = -2;
static uint32_t touch_input_reads = 0;
static bool raw_touch_pressed = false;
static unsigned long last_raw_tap_ms = 0;
static unsigned long title_hold_since = 0;
static bool title_hold_triggered = false;
static PendingAction pending_action = NO_ACTION;
static unsigned long pending_action_since = 0;
static int16_t point_scroll = 0;  // scroll offset for POINT_LIST
static int16_t point_scroll_anchor = 0;
static bool point_scroll_dragging = false;
static bool point_scroll_moved = false;
static bool point_wait_for_release = false;
static uint16_t point_scroll_start_y = 0;
static uint8_t point_scroll_gesture = 0;
static unsigned long point_release_since = 0;
static bool point_list_canvas_ready = false;

#define LOGI(format, ...) do { \
  if (Serial && Serial.availableForWrite() >= 128) { \
    Serial.printf("[panel][%8lu] " format "\n", millis(), ##__VA_ARGS__); \
  } \
} while (0)

static void showPage(Page next);
static bool fetchPoints();
static bool fetchInspections();
static bool createNavigation();
static bool createInspection();
static void handleRawTap(uint16_t x, uint16_t y);
static void renderDirectPage();
static void renderPointListViewport();
static void scheduleAction(PendingAction action);

static String endpoint(const char *path) { return provisioning.backendUrl() + path; }

static const char *pageName(Page value) {
  switch (value) {
    case HOME: return "HOME";
    case POINT_LIST: return "POINT_LIST";
    case CONFIRM_CALL: return "CONFIRM_CALL";
    case WAIT_ARRIVAL: return "WAIT_ARRIVAL";
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
  if (!http.begin(url)) { error_message = "后端地址无效"; LOGI("GET rejected: invalid URL"); return false; }
  addAuth(http);
  int code = http.GET();
  String body = http.getString(); http.end();
  LOGI("GET response code=%d bytes=%u", code, body.length());
  if (code != HTTP_CODE_OK) { error_message = code > 0 ? "后端错误 " + String(code) : "无法连接后端"; LOGI("GET failed: %s", error_message.c_str()); return false; }
  if (deserializeJson(doc, body)) { error_message = "后端响应无效"; LOGI("GET failed: JSON parse error"); return false; }
  return true;
}

static bool postJson(const char *path, JsonDocument &request, JsonDocument &response) {
  HTTPClient http; http.setConnectTimeout(4000); http.setTimeout(5000);
  String url = endpoint(path);
  LOGI("HTTP POST %s", url.c_str());
  if (!http.begin(url)) { error_message = "后端地址无效"; LOGI("POST rejected: invalid URL"); return false; }
  http.addHeader("Content-Type", "application/json"); addAuth(http);
  String body; serializeJson(request, body);
  int code = http.POST(body); String reply = http.getString(); http.end();
  LOGI("POST response code=%d request_bytes=%u response_bytes=%u", code, body.length(), reply.length());
  if (code != HTTP_CODE_OK && code != HTTP_CODE_CREATED) { error_message = code > 0 ? "请求失败 " + String(code) : "无法连接后端"; LOGI("POST failed: %s", error_message.c_str()); return false; }
  if (deserializeJson(response, reply)) { error_message = "后端响应无效"; LOGI("POST failed: JSON parse error"); return false; }
  return true;
}

static String eventId(const char *prefix) {
  return String(PANEL_DEVICE_ID) + "-" + prefix + "-" + String(millis()) + "-" + String(esp_random(), HEX);
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
  if (!point_count) { error_message = "暂无巡检点位"; return false; }
  LOGI("Loaded %u point(s)", point_count);
  return true;
}

static bool fetchInspections() {
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
  LOGI("Loaded %u inspection option(s) for point=%s", inspection_count, points[selected_point].id);
  return true;
}

static void applyTask(JsonDocument &doc) {
  task_id = doc["id"] | ""; task_status = doc["status"] | "queued"; task_message = doc["message"] | "";
  LOGI("Task id=%s status=%s message=%s", task_id.c_str(), task_status.c_str(), task_message.c_str());
}

static bool createNavigation() {
  JsonDocument request, response;
  request["event_id"] = eventId("navigate"); request["device_id"] = PANEL_DEVICE_ID;
  request["site_id"] = PANEL_SITE_ID; request["point_id"] = points[selected_point].id;
  LOGI("Create navigation point=%s event=%s", points[selected_point].id, request["event_id"].as<const char *>());
  if (!postJson("/api/navigation-tasks", request, response)) return false;
  applyTask(response); return task_id.length() > 0;
}

static bool createInspection() {
  JsonDocument request, response;
  request["event_id"] = eventId("inspect"); request["device_id"] = PANEL_DEVICE_ID;
  request["arrival_task_id"] = task_id; request["inspection_id"] = inspections[selected_inspection].id;
  LOGI("Create inspection id=%s arrival_task=%s", inspections[selected_inspection].id, task_id.c_str());
  if (!postJson("/api/inspection-tasks", request, response)) return false;
  applyTask(response); return task_id.length() > 0;
}

static bool pollTask() {
  JsonDocument doc;
  if (!getJson(endpoint("/api/tasks/") + task_id, doc)) return false;
  String previous = task_status;
  task_status = doc["status"] | "failed"; task_message = doc["message"] | "暂无状态";
  if (previous != task_status) LOGI("Task %s status %s -> %s: %s", task_id.c_str(), previous.c_str(), task_status.c_str(), task_message.c_str());
  return true;
}

static lv_obj_t *label(const char *text, lv_align_t align, int x, int y, const lv_font_t *font = nullptr) {
  lv_obj_t *obj = lv_label_create(screen); lv_label_set_text(obj, text);
  lv_obj_set_width(obj, 216); lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
  if (font) lv_obj_set_style_text_font(obj, font, 0);
  lv_obj_align(obj, align, x, y); return obj;
}

static lv_obj_t *button(const char *text, lv_align_t align, int x, int y, lv_event_cb_t callback, void *data = nullptr) {
  lv_obj_t *btn = lv_btn_create(screen); lv_obj_set_size(btn, 204, 34); lv_obj_align(btn, align, x, y);
  lv_obj_add_event_cb(btn, callback, LV_EVENT_CLICKED, data);
  lv_obj_t *txt = lv_label_create(btn); lv_label_set_text(txt, text); lv_obj_center(txt);
  return btn;
}

static void goHome(lv_event_t *) { LOGI("UI: return home"); selected_point = -1; selected_inspection = -1; task_id = ""; showPage(HOME); }
static void retry(lv_event_t *) { LOGI("UI: retry from %s", pageName(page)); showPage(page == ERROR_PAGE ? HOME : page); }
static void selectPoint(lv_event_t *e) { selected_point = (intptr_t)lv_event_get_user_data(e); LOGI("UI: selected point=%s", points[selected_point].id); showPage(CONFIRM_CALL); }
static void confirmCall(lv_event_t *) {
  LOGI("UI: call confirmed");
  task_status = "sending"; task_message = "正在发送机器人呼叫";
  showPage(WAIT_ARRIVAL);
  scheduleAction(SEND_NAVIGATION);
}
static void selectInspection(lv_event_t *e) {
  if (e) selected_inspection = (intptr_t)lv_event_get_user_data(e);
  LOGI("UI: selected inspection=%s", inspections[selected_inspection].id);
  task_status = "sending"; task_message = "正在启动巡检";
  showPage(INSPECTION_STATUS);
  scheduleAction(SEND_INSPECTION);
}

// Fallback dispatcher for the CST816D. It keeps the panel usable if LVGL's
// input timer is not scheduled by the Arduino LVGL build.
static void handleRawTap(uint16_t x, uint16_t y) {
  LOGI("Raw tap x=%u y=%u page=%s", x, y, pageName(page));
  if (page == HOME) {
    // "选择点位" button
    if (y >= 110 && y < 144) {
      if (!provisioning.isConnected() || WiFi.status() != WL_CONNECTED) {
        error_message = "请先连接网络";
        showPage(ERROR_PAGE);
      } else if (point_count == 0) {
        error_message = "点位列表为空";
        showPage(ERROR_PAGE);
      } else {
        point_scroll = 0;
        // Do not reuse the press that opened this page as a list gesture.
        point_wait_for_release = true;
        page = POINT_LIST;
        renderDirectPage();
      }
      return;
    }
    // "配置网络" button
    if (y >= 165 && y < 199) {
      provisioning.startPortal();
      page = NETWORK_SETUP;
      renderDirectPage();
      return;
    }
  } else if (page == POINT_LIST) {
    // Tap on a point button (scrolling handled by drag in loop())
    if (y >= 78 && y <= 230) {
      for (uint8_t i = 0; i < point_count; ++i) {
        int btn_y = 78 + i * 38 - point_scroll;
        if (y >= btn_y && y < btn_y + 34) {
          selected_point = i;
          showPage(CONFIRM_CALL);
          return;
        }
      }
    }
  } else if (page == CONFIRM_CALL) {
    if (y >= 140 && y < 174) { confirmCall(nullptr); return; }
    if (y >= 174) { goHome(nullptr); return; }
  } else if (page == WAIT_ARRIVAL) {
    if (y >= 180) goHome(nullptr);
  } else if (page == PICK_INSPECTION) {
    for (uint8_t i = 0; i < inspection_count; ++i) {
      int top = 88 + i * 36;
      if (y >= top && y < top + 34) { selected_inspection = i; selectInspection(nullptr); return; }
    }
    if (y >= 180) goHome(nullptr);
  } else if (page == INSPECTION_STATUS) {
    if ((task_status == "completed" || task_status == "failed") && y >= 178) goHome(nullptr);
  } else if (page == ERROR_PAGE) {
    if (y >= 140 && y < 174) retry(nullptr);
    else if (y >= 174) goHome(nullptr);
  }
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
  if (status == "running") return "巡检中";
  if (status == "completed") return "已完成";
  if (status == "failed") return "失败";
  return status;
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

static void directButton(const String &text, int top) {
  tft.fillRoundRect(40, top, 160, 34, 5, TFT_DARKGREY);
  tft.drawRoundRect(40, top, 160, 34, 5, TFT_CYAN);
  directText(text, top + 17, 1, TFT_WHITE);
}

static int pointMaxScroll() {
  return max(0, (int)point_count * 38 - (230 - 78));
}

static void renderPointListViewport() {
  if (!point_list_canvas_ready) return;

  point_scroll = constrain(point_scroll, 0, pointMaxScroll());
  point_list_canvas.fillScreen(0);
  point_list_canvas.setFont(&fonts::efontCN_16);
  point_list_canvas.setTextSize(1);
  point_list_canvas.setTextDatum(lgfx::middle_center);
  point_list_canvas.setTextColor(3);

  for (uint8_t i = 0; i < point_count; ++i) {
    int top = 78 + i * 38 - point_scroll - POINT_VIEW_Y;
    if (top >= POINT_VIEW_H || top + 34 <= 0) continue;

    point_list_canvas.fillRoundRect(8, top, 160, 34, 5, 1);
    point_list_canvas.drawRoundRect(8, top, 160, 34, 5, 2);
    String visible = points[i].name;
    bool truncated = point_list_canvas.textWidth(visible) > 148;
    while (visible.length() > 3 && point_list_canvas.textWidth(visible + "...") > 148) {
      removeLastUtf8Character(visible);
    }
    if (truncated) visible += "...";
    point_list_canvas.drawString(visible, POINT_VIEW_W / 2, top + 17);
  }

  // The complete viewport is transferred in one SPI operation, so the cleared
  // background is never visible as an intermediate frame.
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
  for (int y = 0; y < qrSize; ++y) {
    for (int x = 0; x < qrSize; ++x) {
      if (esp_qrcode_get_module(qr, x, y)) tft.fillRect(left + (x + quiet) * scale, top + (y + quiet) * scale, scale, scale, TFT_BLACK);
    }
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
      directText(provisioning.apSsid(), 180, 1, TFT_WHITE);
      directText("扫码加入热点", 198, 1, TFT_WHITE);
      directText("192.168.4.1", 216, 1, TFT_CYAN);
      if (state == PROVISION_FAILED) {
        directText(provisioning.message(), 100, 1, TFT_RED);
      }
    } else if (state == PROVISION_VERIFYING) {
      directText("正在连接 Wi-Fi", 86, 1, TFT_YELLOW);
      directText(provisioning.candidateSsid(), 115, 1, TFT_WHITE);
      directText("请保持手机连接热点", 145, 1, TFT_WHITE);
      directText("192.168.4.1", 190, 1, TFT_CYAN);
    } else if (state == PROVISION_CONNECTING) {
      directText("配置成功", 100, 1, TFT_GREEN);
      directText(provisioning.message(), 140, 1, TFT_WHITE);
      directText("即将重启...", 170, 1, TFT_YELLOW);
    }
  } else if (page == HOME) {
    // 第1行: 设备名称
    directText(PANEL_SITE_NAME, 34, 1, TFT_CYAN);
    // 第2行: Wi-Fi 状态
    if (WiFi.status() == WL_CONNECTED) {
      String status = provisioning.activeSsid() + "  " + WiFi.localIP().toString();
      directText(status, 60, 0, TFT_GREEN);
    } else if (provisioning.isPortal() || provisioning.state() == PROVISION_SCANNING) {
      directText("Wi-Fi 未连接", 60, 1, TFT_RED);
    } else {
      directText("Wi-Fi 连接中...", 60, 1, TFT_YELLOW);
    }
    // 中间: 两个功能入口按钮
    directButton("选择点位", 110);
    directButton("配置网络", 165);
  } else if (page == POINT_LIST) {
    directText("选择巡检点位", 30, 1, TFT_CYAN);
    directText("上下滑动查看更多", 56, 0, TFT_DARKGREY);
    if (point_list_canvas_ready) {
      renderPointListViewport();
    } else {
      point_scroll = constrain(point_scroll, 0, pointMaxScroll());
      for (uint8_t i = 0; i < point_count; ++i) {
        int y_pos = 78 + i * 38 - point_scroll;
        if (y_pos >= 72 && y_pos <= 234) directButton(points[i].name, y_pos);
      }
    }
  } else if (page == CONFIRM_CALL) {
    directText("确认呼叫", 42, 1, TFT_YELLOW);
    directText("机器人前往", 70);
    directText(points[selected_point].name, 94, 1, TFT_CYAN);
    directText("确认后机器人将前往该点位", 116);
    directButton("确认呼叫", 140);
    directButton("返回", 180);
  } else if (page == WAIT_ARRIVAL) {
    directText("机器人状态", 42, 1, TFT_CYAN);
    directText(points[selected_point].name, 70);
    directText(localizedStatus(task_status), 108, 1, TFT_YELLOW);
    directText(task_message, 136);
    directButton("返回", 180);
  } else if (page == PICK_INSPECTION) {
    directText("机器人已到达", 42, 1, TFT_GREEN);
    if (inspection_count == 0) directText("正在加载巡检项目...", 112);
    else {
      directText("请选择巡检项目", 70);
      for (uint8_t i = 0; i < inspection_count; ++i) directButton(inspections[i].name, 88 + i * 36);
    }
    directButton("返回", 180);
  } else if (page == INSPECTION_STATUS) {
    directText("巡检状态", 42, 1, TFT_CYAN);
    directText(selected_inspection >= 0 ? inspections[selected_inspection].name : "巡检项目", 70);
    directText(localizedStatus(task_status), 108, 1, TFT_YELLOW);
    directText(task_message, 136);
    if (task_status == "completed" || task_status == "failed") directButton("完成", 180);
  } else {
    directText("连接错误", 48, 1, TFT_RED);
    directText(error_message, 108);
    directButton("重试", 140);
    directButton("返回首页", 180);
  }
  LOGI("Direct render page=%s elapsed=%lu ms", pageName(page), millis() - started);
}

static void buildHome() {
  label("智巡精灵 v1.0", LV_ALIGN_TOP_MID, 0, 10, &lv_font_montserrat_16);
  String network = WiFi.status() == WL_CONNECTED ? "Wi-Fi connected" : "Wi-Fi offline";
  label(network.c_str(), LV_ALIGN_TOP_MID, 0, 34);
  label("Select an inspection point", LV_ALIGN_TOP_MID, 0, 56);
  if (WiFi.status() != WL_CONNECTED) {
    label("Wi-Fi connecting...", LV_ALIGN_CENTER, 0, 0);
    return;
  }
  if (!fetchPoints()) { label(error_message.c_str(), LV_ALIGN_CENTER, 0, 0); button("Retry", LV_ALIGN_BOTTOM_MID, 0, -14, retry); return; }
  for (uint8_t i = 0; i < point_count; ++i) button(points[i].name, LV_ALIGN_TOP_MID, 0, 82 + i * 38, selectPoint, (void *)(intptr_t)i);
}

static void buildConfirm() {
  label("CONFIRM ROBOT CALL", LV_ALIGN_TOP_MID, 0, 24, &lv_font_montserrat_16);
  label("Call the robot to:", LV_ALIGN_TOP_MID, 0, 64);
  label(points[selected_point].name, LV_ALIGN_TOP_MID, 0, 90, &lv_font_montserrat_16);
  label("The robot will travel to this point.", LV_ALIGN_TOP_MID, 0, 122);
  button("Confirm call", LV_ALIGN_BOTTOM_MID, 0, -54, confirmCall);
  button("Back", LV_ALIGN_BOTTOM_MID, 0, -14, goHome);
}

static void buildWait() {
  label("ROBOT STATUS", LV_ALIGN_TOP_MID, 0, 24, &lv_font_montserrat_16);
  label(points[selected_point].name, LV_ALIGN_TOP_MID, 0, 60);
  label(task_status.c_str(), LV_ALIGN_CENTER, 0, -18, &lv_font_montserrat_16);
  label(task_message.c_str(), LV_ALIGN_CENTER, 0, 18);
  button("Cancel / Back", LV_ALIGN_BOTTOM_MID, 0, -14, goHome);
}

static void buildPickInspection() {
  label("ROBOT ARRIVED", LV_ALIGN_TOP_MID, 0, 22, &lv_font_montserrat_16);
  label("Select an inspection", LV_ALIGN_TOP_MID, 0, 54);
  if (!fetchInspections()) { label(error_message.c_str(), LV_ALIGN_CENTER, 0, 0); button("Back", LV_ALIGN_BOTTOM_MID, 0, -14, goHome); return; }
  for (uint8_t i = 0; i < inspection_count; ++i) button(inspections[i].name, LV_ALIGN_TOP_MID, 0, 86 + i * 40, selectInspection, (void *)(intptr_t)i);
  button("Back", LV_ALIGN_BOTTOM_MID, 0, -14, goHome);
}

static void buildInspectionStatus() {
  label("INSPECTION STATUS", LV_ALIGN_TOP_MID, 0, 24, &lv_font_montserrat_16);
  label(selected_inspection >= 0 ? inspections[selected_inspection].name : "Inspection", LV_ALIGN_TOP_MID, 0, 58);
  label(task_status.c_str(), LV_ALIGN_CENTER, 0, -18, &lv_font_montserrat_16);
  label(task_message.c_str(), LV_ALIGN_CENTER, 0, 18);
  if (task_status == "completed" || task_status == "failed") button("Done", LV_ALIGN_BOTTOM_MID, 0, -14, goHome);
}

static void buildError() {
  label("CONNECTION ERROR", LV_ALIGN_TOP_MID, 0, 34, &lv_font_montserrat_16);
  label(error_message.c_str(), LV_ALIGN_CENTER, 0, -4);
  button("Retry", LV_ALIGN_BOTTOM_MID, 0, -54, retry);
  button("Return home", LV_ALIGN_BOTTOM_MID, 0, -14, goHome);
}

static void showPage(Page next) {
  LOGI("UI page %s -> %s", pageName(page), pageName(next));
  page = next;
  last_poll = millis();
  renderDirectPage();
  if (page == HOME && provisioning.isConnected() && WiFi.status() == WL_CONNECTED && point_count == 0) scheduleAction(LOAD_POINTS);
  if (page == PICK_INSPECTION && inspection_count == 0) scheduleAction(LOAD_INSPECTIONS);
}

static void scheduleAction(PendingAction action) {
  pending_action = action;
  pending_action_since = millis();
  LOGI("Scheduled network action=%d", action);
}

static void runPendingAction() {
  PendingAction action = pending_action;
  pending_action = NO_ACTION;
  bool ok = false;

  if (action == LOAD_POINTS && (page == HOME || page == POINT_LIST)) ok = fetchPoints();
  else if (action == LOAD_INSPECTIONS && page == PICK_INSPECTION) ok = fetchInspections();
  else if (action == SEND_NAVIGATION && page == WAIT_ARRIVAL) ok = createNavigation();
  else if (action == SEND_INSPECTION && page == INSPECTION_STATUS) ok = createInspection();
  else return;

  showPage(ok ? page : ERROR_PAGE);
}

void setup() {
  pinMode(3, OUTPUT); digitalWrite(3, LOW);
  Serial.begin(115200); delay(300);
  LOGI("Boot: Robot inspection panel");
  LOGI("Config: device=%s site=%s backend=%s", PANEL_DEVICE_ID, PANEL_SITE_ID, BACKEND_URL);
  tft.init();
  tft.initDMA();
  tft.fillScreen(TFT_BLACK);
  point_list_canvas.setColorDepth(lgfx::palette_2bit);
  point_list_canvas_ready = point_list_canvas.createSprite(POINT_VIEW_W, POINT_VIEW_H) != nullptr;
  if (point_list_canvas_ready) {
    point_list_canvas.setPaletteColor(0, 0x000000U);
    point_list_canvas.setPaletteColor(1, 0x555555U);
    point_list_canvas.setPaletteColor(2, 0x00FFFFU);
    point_list_canvas.setPaletteColor(3, 0xFFFFFFU);
  }
  LOGI("Point list canvas: %s (%dx%d, 2-bit)", point_list_canvas_ready ? "ready" : "allocation failed", POINT_VIEW_W, POINT_VIEW_H);
  // Keep the GC9A01 SPI transaction open, matching the vendor LVGL example.
  // Re-opening it for the first direct frame can block for several seconds.
  tft.startWrite();
  touch.begin(); lv_init();
  // CST816D uses INT as a startup pulse, then drives it itself.
  pinMode(0, INPUT);
  LOGI("Display initialized; CST816D I2C probe at 0x15: %s", touch.isConnected() ? "found" : "NOT FOUND");
  display = lv_display_create(SCREEN_W, SCREEN_H);
  lv_display_set_buffers(display, draw_a, draw_b, sizeof(draw_a), LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(display, [](lv_display_t *d, const lv_area_t *a, uint8_t *pixels) {
    // Synchronous transfer avoids stale frames observed with GC9A01 DMA.
    tft.startWrite();
    tft.pushImage(a->x1, a->y1, lv_area_get_width(a), lv_area_get_height(a), (lgfx::swap565_t *)pixels);
    tft.endWrite();
    lv_display_flush_ready(d);
  });
  touch_input = lv_indev_create();
  lv_indev_set_type(touch_input, LV_INDEV_TYPE_POINTER);
  lv_indev_set_display(touch_input, display);
  lv_indev_set_read_cb(touch_input, [](lv_indev_t *, lv_indev_data_t *data) { static bool was_pressed = false; uint16_t x, y; uint8_t gesture; ++touch_input_reads; if (touch.getTouch(&x, &y, &gesture)) { data->state = LV_INDEV_STATE_PRESSED; data->point.x = x; data->point.y = y; if (!was_pressed) LOGI("LVGL touch x=%u y=%u gesture=%u", x, y, gesture); was_pressed = true; } else { data->state = LV_INDEV_STATE_RELEASED; was_pressed = false; } });
  screen = lv_obj_create(nullptr); lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE); lv_obj_set_style_pad_all(screen, 8, 0); lv_screen_load(screen);
  provisioning.begin(BACKEND_URL, PANEL_DEVICE_ID);
  showPage(HOME);
  digitalWrite(3, HIGH);
  LOGI("Wi-Fi provisioning state started");
}

void loop() {
  uint16_t raw_x = 0, raw_y = 0;
  uint8_t raw_gesture = 0;
  bool raw_touched = touch.getTouch(&raw_x, &raw_y, &raw_gesture);
  Page page_at_touch_start = page;
  if (raw_touched && !raw_touch_pressed && millis() - last_raw_tap_ms >= 350) {
    // List taps are resolved on release so a drag cannot select its first row.
    if (page_at_touch_start != POINT_LIST) {
      last_raw_tap_ms = millis();
      handleRawTap(raw_x, raw_y);
    }
  }
  raw_touch_pressed = raw_touched;
  // Point list scrolling: distinguish a tap from a drag, then resolve on release.
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
        point_scroll_start_y = raw_y;
        point_scroll_gesture = raw_gesture;
      } else {
        if (raw_gesture) point_scroll_gesture = raw_gesture;
        int16_t delta = (int16_t)point_scroll_start_y - (int16_t)raw_y;
        if (abs(delta) >= 8) {
          point_scroll_moved = true;
          int16_t new_scroll = point_scroll_anchor + delta;
          int max_scroll = (int16_t)point_count * 38 - (230 - 78);
          if (max_scroll < 0) max_scroll = 0;
          if (new_scroll < 0) new_scroll = 0;
          if (new_scroll > max_scroll) new_scroll = max_scroll;
          if (new_scroll != point_scroll) {
            point_scroll = new_scroll;
            if (point_list_canvas_ready) renderPointListViewport();
            else renderDirectPage();
          }
        }
      }
    } else if (point_scroll_dragging) {
      // CST816D can briefly report release while a finger is still down.
      if (!point_release_since) {
        point_release_since = millis();
      } else if (millis() - point_release_since >= 40) {
        if (!point_scroll_moved && (point_scroll_gesture == 1 || point_scroll_gesture == 2)) {
          int max_scroll = (int16_t)point_count * 38 - (230 - 78);
          if (max_scroll < 0) max_scroll = 0;
          point_scroll += point_scroll_gesture == 1 ? 114 : -114;
          if (point_scroll < 0) point_scroll = 0;
          if (point_scroll > max_scroll) point_scroll = max_scroll;
          point_scroll_moved = true;
        }
        bool was_tap = !point_scroll_moved;
        uint16_t tap_y = point_scroll_start_y;
        point_scroll_dragging = false;
        point_release_since = 0;
        if (point_list_canvas_ready) renderPointListViewport();
        else renderDirectPage();
        if (was_tap && millis() - last_raw_tap_ms >= 350) {
          last_raw_tap_ms = millis();
          handleRawTap(0, tap_y);
        }
      }
    }
  } else {
    point_scroll_dragging = false;
    point_release_since = 0;
  }
  if (page == HOME && raw_touched && raw_y <= 64 && !provisioning.isPortal()) {
    if (!title_hold_since) title_hold_since = millis();
    if (!title_hold_triggered && millis() - title_hold_since >= 5000) {
      title_hold_triggered = true;
      provisioning.startPortal();
      page = NETWORK_SETUP;
      renderDirectPage();
    }
  } else {
    title_hold_since = 0;
    title_hold_triggered = false;
  }
  provisioning.loop();
  if (provisioning.needsRender()) {
    if (provisioning.isPortal()) page = NETWORK_SETUP;
    else if (page == NETWORK_SETUP) { page = HOME; point_count = 0; }
    renderDirectPage();
    provisioning.clearRenderRequest();
    if (provisioning.isConnected() && point_count == 0 && WiFi.status() == WL_CONNECTED) scheduleAction(LOAD_POINTS);
  }
  if (pending_action != NO_ACTION && millis() - pending_action_since >= 50) runPendingAction();
  if (page == HOME && provisioning.isConnected() && point_count == 0 && WiFi.status() == WL_CONNECTED && millis() - last_home_refresh >= 1000) {
    last_home_refresh = millis();
    LOGI("Wi-Fi connected IP=%s RSSI=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    showPage(HOME);
  }
  if (millis() - last_touch_diagnostic >= 250) {
    last_touch_diagnostic = millis();
    int count = touch.touchCount();
    if (count != previous_touch_count) {
      LOGI("CST816D raw touch count=%d", count);
      if (count > 0) {
        uint16_t x = 0, y = 0;
        uint8_t gesture = 0;
        if (touch.getTouch(&x, &y, &gesture)) LOGI("CST816D raw point x=%u y=%u gesture=%u", x, y, gesture);
        else LOGI("CST816D raw point read failed");
      }
      previous_touch_count = count;
    }
  }
  if ((page == WAIT_ARRIVAL || page == INSPECTION_STATUS) && millis() - last_poll >= POLL_INTERVAL_MS) {
    last_poll = millis();
    LOGI("Polling task=%s", task_id.c_str());
    if (!pollTask()) showPage(ERROR_PAGE);
    else if (page == WAIT_ARRIVAL && task_status == "arrived") showPage(PICK_INSPECTION);
    else if (task_status == "failed" || task_status == "completed") showPage(page);
    else showPage(page);
  }
  delay(5);
}
