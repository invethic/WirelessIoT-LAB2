// ============================================================================
//  CPLab Tracking - common layer shared by every Tab5 role
//  Festo CP Lab 400 "smartphone cases" line - Production & Maintenance tracking
//
//  Board   : M5Stack Tab5 (ESP32-P4 + ESP32-C6 for Wi-Fi), m5stack esp32 core 3.2.x
//  Libs    : M5Unified, M5GFX, PubSubClient (Nick O'Leary), ArduinoJson 7.x
//
//  This file is IDENTICAL in every sketch folder. Edit it once, copy it everywhere.
//
//  MQTT conventions (TOPIC_ROOT = "cplab"):
//    cplab/status/<devId>          "online"/"offline"   (retained, LWT)
//    cplab/hb/<devId>              heartbeat JSON every 10 s
//    cplab/cmd/<target>/msg        {"text","from","level":"info|warn|alarm","duration":s}
//    cplab/cmd/<target>/action     {"cmd":"reboot|identify|reset|..."}
//    cplab/ack/<devId>             acknowledgement of a displayed message
//    <target> = devId (e.g. prod-st2) | group ("prod" / "maint") | "all"
// ============================================================================
#pragma once
#include <M5Unified.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "config.h"

#ifndef FW_VERSION
#define FW_VERSION "1.0.0"
#endif

// --- Tab5: SDIO link between ESP32-P4 and the ESP32-C6 Wi-Fi co-processor ---
#define SDIO2_CLK GPIO_NUM_12
#define SDIO2_CMD GPIO_NUM_13
#define SDIO2_D0  GPIO_NUM_11
#define SDIO2_D1  GPIO_NUM_10
#define SDIO2_D2  GPIO_NUM_9
#define SDIO2_D3  GPIO_NUM_8
#define SDIO2_RST GPIO_NUM_15

// ============================================================================
//  UI palette & helpers  (no accented characters: GFX free fonts are ASCII)
// ============================================================================
namespace ui {
constexpr uint16_t c565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
constexpr uint16_t BG    = c565(230, 233, 236);  // anodised aluminium
constexpr uint16_t PANEL = c565(255, 255, 255);
constexpr uint16_t INK   = c565(22, 33, 43);
constexpr uint16_t MUTED = c565(96, 108, 118);
constexpr uint16_t LINE  = c565(196, 202, 208);
constexpr uint16_t BLUE  = c565(10, 138, 208);   // Festo-like blue
constexpr uint16_t GREEN = c565(31, 157, 85);
constexpr uint16_t AMBER = c565(232, 163, 23);
constexpr uint16_t RED   = c565(207, 59, 59);
constexpr uint16_t GREY  = c565(140, 150, 159);
constexpr uint16_t WHITE = c565(255, 255, 255);

int W = 1280, H = 720;
constexpr int HEADER_H = 72;

struct Button {
  int x, y, w, h;
  const char* label;
  uint16_t color;
  bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  void draw(bool active = false) const {
    uint16_t bg = active ? INK : color;
    M5.Display.fillRoundRect(x, y, w, h, 10, bg);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(WHITE, bg);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString(label, x + w / 2, y + h / 2);
  }
};

inline void panel(int x, int y, int w, int h, const char* title = nullptr) {
  M5.Display.fillRoundRect(x, y, w, h, 8, PANEL);
  M5.Display.drawRoundRect(x, y, w, h, 8, LINE);
  if (title) {
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(MUTED, PANEL);
    M5.Display.setTextDatum(top_left);
    M5.Display.drawString(title, x + 16, y + 12);
  }
}

inline void text(const String& s, int x, int y, const lgfx::IFont* f, uint16_t fg, uint16_t bg,
                 textdatum_t datum = top_left, float size = 1.0f) {
  M5.Display.setFont(f);
  M5.Display.setTextSize(size);
  M5.Display.setTextColor(fg, bg);
  M5.Display.setTextDatum(datum);
  M5.Display.drawString(s, x, y);
  M5.Display.setTextSize(1);
}
}  // namespace ui

// ============================================================================
//  Network layer
// ============================================================================
namespace cp {
WiFiClient net;
PubSubClient mqtt(net);
String devId, devRole, devGroup, devTitle;

// hooks set by each sketch
void (*onMessage)(const String& topic, const String& payload) = nullptr;
void (*onSubscribe)() = nullptr;
void (*onAction)(JsonDocument& d) = nullptr;

bool requestRedraw = false;     // sketch must redraw the full screen when true
uint32_t identifyUntil = 0;

struct Overlay {
  String text, from, level;
  uint32_t until = 0;
  bool visible = false, dirty = false;
} ov;

inline String topic(const String& sub) { return String(TOPIC_ROOT) + "/" + sub; }

inline bool publish(const String& sub, const String& payload, bool retain = false) {
  if (!mqtt.connected()) return false;
  return mqtt.publish(topic(sub).c_str(), payload.c_str(), retain);
}
inline bool publishJson(const String& sub, JsonDocument& d, bool retain = false) {
  String s;
  serializeJson(d, s);
  return publish(sub, s, retain);
}
inline void subscribe(const String& sub) { mqtt.subscribe(topic(sub).c_str(), 1); }

inline bool canDraw() { return !ov.visible; }

void heartbeat() {
  JsonDocument d;
  d["id"] = devId;
  d["role"] = devRole;
  d["group"] = devGroup;
  d["title"] = devTitle;
  d["ip"] = WiFi.localIP().toString();
  d["rssi"] = WiFi.RSSI();
  d["up"] = millis() / 1000;
  d["fw"] = FW_VERSION;
  d["heap"] = ESP.getFreeHeap();
  publishJson("hb/" + devId, d);
}

void callback(char* t, byte* p, unsigned int n) {
  String tp(t), pl;
  pl.reserve(n);
  for (unsigned i = 0; i < n; i++) pl += (char)p[i];

  const String cmdRoot = String(TOPIC_ROOT) + "/cmd/";
  if (tp.startsWith(cmdRoot)) {
    String rest = tp.substring(cmdRoot.length());          // "<target>/<kind>"
    int s = rest.indexOf('/');
    if (s < 0) return;
    String target = rest.substring(0, s), kind = rest.substring(s + 1);
    if (target != "all" && target != devId && target != devGroup) return;

    JsonDocument d;
    bool bad = deserializeJson(d, pl) != DeserializationError::Ok;
    if (kind == "msg") {
      uint32_t dur = 15;
      if (bad) { ov.text = pl; ov.from = "Superviseur"; ov.level = "info"; }
      else {
        ov.text  = d["text"]  | "";
        ov.from  = d["from"]  | "Superviseur";
        ov.level = d["level"] | "info";
        dur      = d["duration"] | 15;
      }
      ov.until = millis() + dur * 1000UL;
      ov.visible = true;
      ov.dirty = true;
      M5.Speaker.tone(ov.level == "alarm" ? 2600 : 1800, 150);
      JsonDocument a;
      a["id"] = devId;
      a["text"] = ov.text;
      publishJson("ack/" + devId, a);
    } else if (kind == "action" && !bad) {
      String cmd = d["cmd"] | "";
      if (cmd == "reboot") { delay(300); ESP.restart(); }
      else if (cmd == "identify") { identifyUntil = millis() + 5000; }
      else if (onAction) onAction(d);
    }
    return;
  }
  if (onMessage) onMessage(tp, pl);
}

uint32_t lastWifiTry = 0, lastMqttTry = 0, lastHb = 0;

void begin(const String& id, const String& role, const String& group, const String& title) {
  devId = id; devRole = role; devGroup = group; devTitle = title;
  WiFi.setPins(SDIO2_CLK, SDIO2_CMD, SDIO2_D0, SDIO2_D1, SDIO2_D2, SDIO2_D3, SDIO2_RST);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lastWifiTry = millis();
  mqtt.setServer(BROKER_HOST, BROKER_PORT);
  mqtt.setCallback(callback);
  mqtt.setBufferSize(2048);
  mqtt.setKeepAlive(15);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiTry > 8000) {
      lastWifiTry = millis();
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    return;
  }
  if (!mqtt.connected()) {
    if (millis() - lastMqttTry > 3000) {
      lastMqttTry = millis();
      String cid = devId + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
      String will = topic("status/" + devId);
      const char* u = MQTT_USER[0] ? MQTT_USER : nullptr;
      const char* pw = MQTT_PASS[0] ? MQTT_PASS : nullptr;
      if (mqtt.connect(cid.c_str(), u, pw, will.c_str(), 1, true, "offline")) {
        mqtt.publish(will.c_str(), "online", true);
        subscribe("cmd/all/#");
        subscribe("cmd/" + devId + "/#");
        subscribe("cmd/" + devGroup + "/#");
        if (onSubscribe) onSubscribe();
        heartbeat();
        lastHb = millis();
      }
    }
    return;
  }
  mqtt.loop();
  if (millis() - lastHb > 10000) { lastHb = millis(); heartbeat(); }
}

// ----------------------------------------------------------------------------
//  Header with Wi-Fi / MQTT indicators
// ----------------------------------------------------------------------------
int lastLink = -1;
inline int linkState() { return (WiFi.status() == WL_CONNECTED ? 1 : 0) + (mqtt.connected() ? 2 : 0); }

void drawLink() {
  int s = linkState();
  int x = ui::W - 300;
  M5.Display.fillRect(x, 0, 300, ui::HEADER_H, ui::INK);
  M5.Display.fillCircle(x + 40, 36, 10, (s & 1) ? ui::GREEN : ui::RED);
  ui::text("WiFi", x + 58, 36, &fonts::FreeSans12pt7b, ui::WHITE, ui::INK, middle_left);
  M5.Display.fillCircle(x + 160, 36, 10, (s & 2) ? ui::GREEN : ui::RED);
  ui::text("MQTT", x + 178, 36, &fonts::FreeSans12pt7b, ui::WHITE, ui::INK, middle_left);
  lastLink = s;
}

void drawHeader(const String& title, const String& sub) {
  M5.Display.fillRect(0, 0, ui::W, ui::HEADER_H, ui::INK);
  ui::text(title, 24, 36, &fonts::FreeSansBold18pt7b, ui::WHITE, ui::INK, middle_left);
  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  int tw = M5.Display.textWidth(title);
  ui::text(sub, 24 + tw + 28, 38, &fonts::FreeSans12pt7b, ui::c565(170, 190, 205), ui::INK, middle_left);
  drawLink();
}

// ----------------------------------------------------------------------------
//  Supervisor message overlay (tap to close, auto-close after duration)
// ----------------------------------------------------------------------------
void drawOverlay() {
  uint16_t c = ov.level == "alarm" ? ui::RED : ov.level == "warn" ? ui::AMBER : ui::BLUE;
  int w = 900, h = 360, x = (ui::W - w) / 2, y = (ui::H - h) / 2;
  M5.Display.fillRoundRect(x - 6, y - 6, w + 12, h + 12, 16, ui::INK);
  M5.Display.fillRoundRect(x, y, w, h, 12, ui::PANEL);
  M5.Display.fillRoundRect(x, y, w, 70, 12, c);
  M5.Display.fillRect(x, y + 50, w, 20, c);
  ui::text("Message de " + ov.from, x + 24, y + 35, &fonts::FreeSansBold18pt7b, ui::WHITE, c, middle_left);
  // simple word wrap
  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  M5.Display.setTextColor(ui::INK, ui::PANEL);
  M5.Display.setTextDatum(top_left);
  int cx = x + 30, cy = y + 100, maxW = w - 60;
  String line, word, src = ov.text + " ";
  for (unsigned i = 0; i < src.length() && cy < y + h - 80; i++) {
    char ch = src[i];
    if (ch != ' ' && ch != '\n') { word += ch; continue; }
    String test = line.length() ? line + " " + word : word;
    if (M5.Display.textWidth(test) > maxW && line.length()) {
      M5.Display.drawString(line, cx, cy); cy += 46; line = word;
    } else line = test;
    word = "";
    if (ch == '\n') { M5.Display.drawString(line, cx, cy); cy += 46; line = ""; }
  }
  if (line.length() && cy < y + h - 80) M5.Display.drawString(line, cx, cy);
  ui::text("Toucher pour fermer", x + w / 2, y + h - 30, &fonts::FreeSans12pt7b, ui::MUTED, ui::PANEL, middle_center);
}

// returns true if the touch was consumed by the overlay
inline bool overlayTouch() {
  if (!ov.visible) return false;
  ov.visible = false;
  requestRedraw = true;
  return true;
}

void service() {
  if (ov.visible && ov.dirty) { drawOverlay(); ov.dirty = false; }
  if (ov.visible && millis() > ov.until) { ov.visible = false; requestRedraw = true; }
  static bool idOn = false;
  bool idNow = millis() < identifyUntil;
  if (idNow && canDraw()) {
    uint16_t c = ((millis() / 300) % 2) ? ui::BLUE : ui::AMBER;
    for (int i = 0; i < 8; i++) M5.Display.drawRect(i, ui::HEADER_H + i, ui::W - 2 * i, ui::H - ui::HEADER_H - 2 * i, c);
    idOn = true;
  } else if (idOn && !idNow) { idOn = false; requestRedraw = true; }
  if (linkState() != lastLink && canDraw()) drawLink();
}

// Common M5 init (landscape 1280x720)
void initDisplay() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(3);          // landscape; use 1 if the image is upside down
  ui::W = M5.Display.width();
  ui::H = M5.Display.height();
  M5.Touch.setHoldThresh(1500);       // "maintenir" buttons = 1.5 s press
  M5.Speaker.setVolume(120);
  M5.Display.fillScreen(ui::BG);
}
}  // namespace cp
