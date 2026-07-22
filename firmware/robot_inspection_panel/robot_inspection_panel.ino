#define LGFX_USE_V1

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <lvgl.h>
#include <LovyanGFX.hpp>

#include "CST816D.h"
#include "config.h"

static constexpr uint16_t SCREEN_W = 240;
static constexpr uint16_t SCREEN_H = 240;
static constexpr uint8_t DRAW_ROWS = 24;
static constexpr uint8_t MAX_POINTS = 6;
static constexpr uint8_t MAX_INSPECTIONS = 4;
static constexpr unsigned long POLL_INTERVAL_MS = 1000;

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
enum Page { HOME, CONFIRM_CALL, WAIT_ARRIVAL, PICK_INSPECTION, INSPECTION_STATUS, ERROR_PAGE };

LGFX tft;
CST816D touch(4, 5, 1, 0);
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
static bool direct_dirty = true;

#define LOGI(format, ...) Serial.printf("[panel][%8lu] " format "\n", millis(), ##__VA_ARGS__)

static void showPage(Page next);
static bool fetchPoints();
static bool fetchInspections();
static bool createNavigation();
static bool createInspection();
static void handleRawTap(uint16_t x, uint16_t y);
static void renderDirectPage();

static String endpoint(const char *path) { return String(BACKEND_URL) + path; }

static const char *pageName(Page value) {
  switch (value) {
    case HOME: return "HOME";
    case CONFIRM_CALL: return "CONFIRM_CALL";
    case WAIT_ARRIVAL: return "WAIT_ARRIVAL";
    case PICK_INSPECTION: return "PICK_INSPECTION";
    case INSPECTION_STATUS: return "INSPECTION_STATUS";
    case ERROR_PAGE: return "ERROR";
  }
  return "UNKNOWN";
}

static void addAuth(HTTPClient &http) {
  if (strlen(API_TOKEN)) http.addHeader("X-API-Token", API_TOKEN);
}

static bool getJson(const String &url, JsonDocument &doc) {
  HTTPClient http; http.setConnectTimeout(4000); http.setTimeout(5000);
  LOGI("HTTP GET %s", url.c_str());
  if (!http.begin(url)) { error_message = "Invalid backend URL"; LOGI("GET rejected: invalid URL"); return false; }
  addAuth(http);
  int code = http.GET();
  String body = http.getString(); http.end();
  LOGI("GET response code=%d bytes=%u", code, body.length());
  if (code != HTTP_CODE_OK) { error_message = code > 0 ? "Backend HTTP " + String(code) : "Backend unreachable"; LOGI("GET failed: %s", error_message.c_str()); return false; }
  if (deserializeJson(doc, body)) { error_message = "Invalid backend response"; LOGI("GET failed: JSON parse error"); return false; }
  return true;
}

static bool postJson(const char *path, JsonDocument &request, JsonDocument &response) {
  HTTPClient http; http.setConnectTimeout(4000); http.setTimeout(5000);
  String url = endpoint(path);
  LOGI("HTTP POST %s", url.c_str());
  if (!http.begin(url)) { error_message = "Invalid backend URL"; LOGI("POST rejected: invalid URL"); return false; }
  http.addHeader("Content-Type", "application/json"); addAuth(http);
  String body; serializeJson(request, body);
  int code = http.POST(body); String reply = http.getString(); http.end();
  LOGI("POST response code=%d request_bytes=%u response_bytes=%u", code, body.length(), reply.length());
  if (code != HTTP_CODE_OK && code != HTTP_CODE_CREATED) { error_message = code > 0 ? "Request failed (" + String(code) + ")" : "Backend unreachable"; LOGI("POST failed: %s", error_message.c_str()); return false; }
  if (deserializeJson(response, reply)) { error_message = "Invalid backend response"; LOGI("POST failed: JSON parse error"); return false; }
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
    strlcpy(points[point_count].name, item["name"] | "Unnamed point", sizeof(points[0].name));
    ++point_count;
  }
  if (!point_count) { error_message = "No inspection points"; return false; }
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
    strlcpy(inspections[inspection_count].name, item["name"] | "Inspection", sizeof(inspections[0].name));
    ++inspection_count;
  }
  if (!inspection_count) { error_message = "No inspections available"; return false; }
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
  task_status = doc["status"] | "failed"; task_message = doc["message"] | "No status";
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
  task_status = "sending"; task_message = "Sending robot call";
  showPage(WAIT_ARRIVAL);
  if (createNavigation()) showPage(WAIT_ARRIVAL); else showPage(ERROR_PAGE);
}
static void selectInspection(lv_event_t *e) {
  selected_inspection = (intptr_t)lv_event_get_user_data(e); LOGI("UI: selected inspection=%s", inspections[selected_inspection].id);
  task_status = "sending"; task_message = "Starting inspection";
  showPage(INSPECTION_STATUS);
  if (createInspection()) showPage(INSPECTION_STATUS); else showPage(ERROR_PAGE);
}

// Fallback dispatcher for the CST816D. It keeps the panel usable if LVGL's
// input timer is not scheduled by the Arduino LVGL build.
static void handleRawTap(uint16_t x, uint16_t y) {
  LOGI("Raw tap x=%u y=%u page=%s", x, y, pageName(page));
  if (page == HOME) {
    for (uint8_t i = 0; i < point_count; ++i) {
      int top = 82 + i * 38;
      if (y >= top && y < top + 34) { selected_point = i; showPage(CONFIRM_CALL); return; }
    }
  } else if (page == CONFIRM_CALL) {
    if (y >= 140 && y < 182) { confirmCall(nullptr); return; }
    if (y >= 182) { goHome(nullptr); return; }
  } else if (page == WAIT_ARRIVAL) {
    if (y >= 178) goHome(nullptr);
  } else if (page == PICK_INSPECTION) {
    for (uint8_t i = 0; i < inspection_count; ++i) {
      int top = 86 + i * 40;
      if (y >= top && y < top + 34) { selected_inspection = i; selectInspection(nullptr); return; }
    }
    if (y >= 178) goHome(nullptr);
  } else if (page == INSPECTION_STATUS) {
    if ((task_status == "completed" || task_status == "failed") && y >= 178) goHome(nullptr);
  } else if (page == ERROR_PAGE) {
    if (y >= 140 && y < 182) retry(nullptr);
    else if (y >= 182) goHome(nullptr);
  }
}

static void directText(const String &text, int y, uint8_t size = 1, uint16_t color = TFT_WHITE) {
  tft.setTextSize(size);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextDatum(lgfx::middle_center);
  tft.drawString(text, SCREEN_W / 2, y);
}

static void directButton(const String &text, int top) {
  tft.fillRoundRect(10, top, 220, 34, 5, TFT_DARKGREY);
  tft.drawRoundRect(10, top, 220, 34, 5, TFT_CYAN);
  directText(text, top + 17, 1, TFT_WHITE);
}

static void renderDirectPage() {
  tft.startWrite();
  tft.fillScreen(TFT_BLACK);
  if (page == HOME) {
    directText("ROBOT INSPECTION", 16, 2, TFT_CYAN);
    directText(WiFi.status() == WL_CONNECTED ? "Wi-Fi connected" : "Wi-Fi connecting...", 39);
    if (WiFi.status() != WL_CONNECTED) directText("Connecting to network", 112);
    else if (point_count == 0) directText("Loading points...", 112);
    else {
      directText("Select an inspection point", 61);
      for (uint8_t i = 0; i < point_count; ++i) directButton(points[i].name, 82 + i * 38);
    }
  } else if (page == CONFIRM_CALL) {
    directText("CONFIRM ROBOT CALL", 24, 2, TFT_YELLOW);
    directText("Call robot to:", 66);
    directText(points[selected_point].name, 91, 2, TFT_CYAN);
    directText("Robot will travel to this point", 122);
    directButton("Confirm call", 144);
    directButton("Back", 184);
  } else if (page == WAIT_ARRIVAL) {
    directText("ROBOT STATUS", 24, 2, TFT_CYAN);
    directText(points[selected_point].name, 60);
    directText(task_status, 106, 2, TFT_YELLOW);
    directText(task_message, 136);
    directButton("Cancel / Back", 184);
  } else if (page == PICK_INSPECTION) {
    directText("ROBOT ARRIVED", 22, 2, TFT_GREEN);
    directText("Select an inspection", 54);
    for (uint8_t i = 0; i < inspection_count; ++i) directButton(inspections[i].name, 86 + i * 40);
    directButton("Back", 184);
  } else if (page == INSPECTION_STATUS) {
    directText("INSPECTION STATUS", 24, 2, TFT_CYAN);
    directText(selected_inspection >= 0 ? inspections[selected_inspection].name : "Inspection", 58);
    directText(task_status, 106, 2, TFT_YELLOW);
    directText(task_message, 136);
    if (task_status == "completed" || task_status == "failed") directButton("Done", 184);
  } else {
    directText("CONNECTION ERROR", 34, 2, TFT_RED);
    directText(error_message, 112);
    directButton("Retry", 144);
    directButton("Return home", 184);
  }
  tft.endWrite();
}

static void buildHome() {
  label("ROBOT INSPECTION", LV_ALIGN_TOP_MID, 0, 10, &lv_font_montserrat_16);
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
  if (page == HOME && WiFi.status() == WL_CONNECTED && point_count == 0 && !fetchPoints()) page = ERROR_PAGE;
  if (page == PICK_INSPECTION && inspection_count == 0 && !fetchInspections()) page = ERROR_PAGE;
  renderDirectPage();
}

void setup() {
  Serial.begin(115200); delay(300);
  LOGI("Boot: Robot inspection panel");
  LOGI("Config: device=%s site=%s backend=%s", PANEL_DEVICE_ID, PANEL_SITE_ID, BACKEND_URL);
  pinMode(3, OUTPUT); digitalWrite(3, HIGH);
  tft.init();
  tft.fillScreen(TFT_BLACK);
  touch.begin(); lv_init();
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
  LOGI("Wi-Fi connecting to SSID '%s'", WIFI_SSID); WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  screen = lv_obj_create(nullptr); lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE); lv_obj_set_style_pad_all(screen, 8, 0); lv_screen_load(screen);
  showPage(HOME);
  lv_refr_now(display);
}

void loop() {
  uint16_t raw_x = 0, raw_y = 0;
  uint8_t raw_gesture = 0;
  bool raw_touched = touch.getTouch(&raw_x, &raw_y, &raw_gesture);
  if (raw_touched && !raw_touch_pressed && millis() - last_raw_tap_ms >= 350) {
    last_raw_tap_ms = millis();
    handleRawTap(raw_x, raw_y);
  }
  raw_touch_pressed = raw_touched;
  if (page == HOME && point_count == 0 && WiFi.status() == WL_CONNECTED && millis() - last_home_refresh >= 1000) {
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
