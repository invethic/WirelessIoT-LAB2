// ============================================================================
//  CPLab_Maint  -  Tab5 maintenance node (M1..M4)
//
//  Vibration sensor sampled at 1 kHz (esp_timer). Every second:
//    activity = max(transitions/s, % time active)   (digital DO)
//    or RMS of the AC part of the signal            (analog AO, if wired)
//  State machine with confirmation delays:
//    OFF       activity < th_off
//    ABNORMAL  activity > th_high   (excessive)  or  std/mean > CV_ABNORMAL (irregular)
//    RUN       otherwise
//  Publishes   cplab/maint/mN/state  (retained, on change + every 5 s)
//              cplab/maint/mN/ack    (operator acknowledgement)
//  Listens     cplab/maint/mN/stats  (from Node-RED: breakdowns 24 h / 7 days,
//                                     MTBF/MTTR, next preventive / curative action)
//  Remote      cplab/cmd/maint-mN/action {"cmd":"learn"|"thresholds"|"sim_fault"|"sim_stop"|"sim_end"}
// ============================================================================
#include "cplab_common.h"
#include "esp_timer.h"

// Declared before the first function (Arduino auto-prototypes are inserted there)
enum State { S_RUN, S_OFF, S_ABN };

const String KEY = "m" + String(MACHINE_NUM);
const String DEV = "maint-" + KEY;

// ---------------------------------------------------------------- sampling -
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t sEdges = 0, sActive = 0, sTotal = 0;
volatile double sSum = 0, sSum2 = 0;
volatile int lastLevel = 0;

void sampleCb(void*) {          // esp_timer task context
  int lv = digitalRead(VIB_PIN);
  int active = VIB_ACTIVE_LOW ? !lv : lv;
  int a = (VIB_AO_PIN >= 0) ? analogRead(VIB_AO_PIN) : 0;
  portENTER_CRITICAL(&mux);
  if (lv != lastLevel) { sEdges++; lastLevel = lv; }
  sActive += active;
  sTotal++;
  sSum += a; sSum2 += (double)a * a;
  portEXIT_CRITICAL(&mux);
}

// ---------------------------------------------------------------- state ----
const char* ST_CODE[3] = {"RUN", "OFF", "ABNORMAL"};
const char* ST_FR[3]   = {"EN MARCHE", "ARRET", "ANOMALIE"};
const uint16_t ST_COL[3] = {ui::GREEN, ui::GREY, ui::RED};

State state = S_OFF, pending = S_OFF;
int pendingCnt = 0;
String reason = "";
uint32_t sinceMs = 0, abnBoot = 0;
float activity = 0, ratio = 0, cv = 0;
float hist[60] = {0}; int histI = 0;
float thOff = TH_OFF_DEFAULT, thHigh = TH_HIGH_DEFAULT;

int simMode = 0; uint32_t simUntil = 0;           // 1 = fault, 2 = stop
int learnLeft = 0; float learnBuf[10];

// stats from Node-RED
int bDay = -1, bWeek = -1;
float mtbf = -1, mttr = -1;
String prevDate, prevTask, curDate, curTask;

Preferences prefs;
uint32_t lastPub = 0;

void publishState() {
  JsonDocument d;
  d["m"] = MACHINE_NUM; d["id"] = KEY; d["name"] = MACHINE_NAME;
  d["state"] = ST_CODE[state];
  d["reason"] = reason;
  d["activity"] = roundf(activity * 10) / 10.0f;
  d["ratio"] = roundf(ratio * 100) / 100.0f;
  d["cv"] = roundf(cv * 100) / 100.0f;
  d["th_off"] = thOff; d["th_high"] = thHigh;
  d["since_s"] = (millis() - sinceMs) / 1000;
  d["abn_boot"] = abnBoot;
  d["sim"] = simMode != 0;
  cp::publishJson("maint/" + KEY + "/state", d, true);
  lastPub = millis();
}

void setState(State s, const String& why) {
  state = s; reason = why; sinceMs = millis();
  if (s == S_ABN) { abnBoot++; M5.Speaker.tone(2600, 300); }
  publishState();
}

void evaluate() {                 // called every second
  uint32_t e, a, n; double sum, sum2;
  portENTER_CRITICAL(&mux);
  e = sEdges; a = sActive; n = sTotal; sum = sSum; sum2 = sSum2;
  sEdges = sActive = sTotal = 0; sSum = sSum2 = 0;
  portEXIT_CRITICAL(&mux);
  if (!n) return;

  ratio = (float)a / n;
  if (VIB_AO_PIN >= 0) {
    double mean = sum / n;
    activity = sqrt(fmax(0.0, sum2 / n - mean * mean)) / 10.0;      // AC RMS (ADC counts / 10)
  } else {
    float tps = e * 1000.0f / n;                                      // transitions per second
    activity = fmaxf(tps, ratio * 100.0f);
  }
  // training simulation overrides the measure
  if (simMode && millis() > simUntil) simMode = 0;
  if (simMode == 1) activity = thHigh * (1.4f + random(0, 60) / 100.0f);
  if (simMode == 2) activity = 0;

  hist[histI] = activity; histI = (histI + 1) % 60;

  // irregularity over the last 10 s
  float m = 0, v = 0;
  for (int i = 1; i <= 10; i++) m += hist[(histI - i + 60) % 60];
  m /= 10;
  for (int i = 1; i <= 10; i++) { float d = hist[(histI - i + 60) % 60] - m; v += d * d; }
  cv = m > 0.01f ? sqrtf(v / 10) / m : 0;

  // learning
  if (learnLeft > 0) {
    learnBuf[10 - learnLeft] = activity;
    if (--learnLeft == 0) {
      float mm = 0, sd = 0;
      for (float x : learnBuf) mm += x;
      mm /= 10;
      for (float x : learnBuf) sd += (x - mm) * (x - mm);
      sd = sqrtf(sd / 10);
      thOff = fmaxf(0.5f, 0.25f * mm);
      thHigh = fmaxf(mm + 5.0f, 2.0f * mm + 3.0f * sd);
      prefs.putFloat("thOff", thOff); prefs.putFloat("thHigh", thHigh);
      cp::requestRedraw = true;
    }
  }

  // candidate state
  State cand; String why;
  if (activity < thOff) { cand = S_OFF; why = ""; }
  else if (activity > thHigh) { cand = S_ABN; why = "Vibrations excessives"; }
  else if (cv > CV_ABNORMAL && m > thOff) { cand = S_ABN; why = "Vibrations irregulieres"; }
  else { cand = S_RUN; why = ""; }

  if (cand == state) { pendingCnt = 0; return; }
  if (cand != pending) { pending = cand; pendingCnt = 0; }
  pendingCnt++;
  int need = cand == S_OFF ? OFF_CONFIRM_S : cand == S_ABN ? ABN_CONFIRM_S : 2;
  if (pendingCnt >= need) { pendingCnt = 0; setState(cand, why); }
}

// ---------------------------------------------------------------- MQTT -----
void onMsg(const String& t, const String& p) {
  if (t != cp::topic("maint/" + KEY + "/stats")) return;
  JsonDocument d;
  if (deserializeJson(d, p)) return;
  bDay = d["day"] | -1;
  bWeek = d["week"] | -1;
  mtbf = d["mtbf_h"].isNull() ? -1 : d["mtbf_h"].as<float>();
  mttr = d["mttr_min"].isNull() ? -1 : d["mttr_min"].as<float>();
  prevDate = (const char*)(d["next_prev"]["date"] | "");
  prevTask = (const char*)(d["next_prev"]["task"] | "");
  curDate = (const char*)(d["next_cur"]["date"] | "");
  curTask = (const char*)(d["next_cur"]["task"] | "");
}
void onSub() { cp::subscribe("maint/" + KEY + "/stats"); publishState(); }
void onAct(JsonDocument& d) {
  String c = d["cmd"] | "";
  if (c == "learn") learnLeft = 10;
  else if (c == "thresholds") {
    thOff = d["off"] | thOff; thHigh = d["high"] | thHigh;
    prefs.putFloat("thOff", thOff); prefs.putFloat("thHigh", thHigh);
  }
  else if (c == "sim_fault") { simMode = 1; simUntil = millis() + (uint32_t)(d["duration"] | 20) * 1000; }
  else if (c == "sim_stop")  { simMode = 2; simUntil = millis() + (uint32_t)(d["duration"] | 20) * 1000; }
  else if (c == "sim_end")   simMode = 0;
}

void acknowledge() {
  JsonDocument d;
  d["id"] = KEY; d["by"] = "operateur"; d["state"] = ST_CODE[state];
  cp::publishJson("maint/" + KEY + "/ack", d);
  if (simMode == 1) simMode = 0;
  M5.Speaker.tone(1600, 80);
}

// ---------------------------------------------------------------- UI -------
ui::Button bAck   = {20, 612, 295, 88, "Acquitter", ui::BLUE};
ui::Button bLearn = {335, 612, 295, 88, "Apprendre normal", ui::MUTED};
ui::Button bFault = {650, 612, 295, 88, "Simuler panne", ui::RED};
ui::Button bStop  = {965, 612, 295, 88, "Simuler arret", ui::GREY};
String sigS, sigG, sigR;

String dur(uint32_t s) {
  char b[32];
  if (s < 3600) snprintf(b, sizeof b, "%u min %02u s", (unsigned)(s / 60), (unsigned)(s % 60));
  else snprintf(b, sizeof b, "%u h %02u min", (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
  return b;
}

void drawStatic() {
  M5.Display.fillScreen(ui::BG);
  cp::drawHeader("Machine " + String(MACHINE_NUM) + " - " MACHINE_NAME, "Maintenance");
  ui::panel(20, 348, 600, 244, "Niveau vibratoire (60 s)");
  ui::panel(640, 92, 620, 500, "Historique et plan de maintenance");
  bAck.draw(); bLearn.draw(); bFault.draw(); bStop.draw();
  sigS = sigG = sigR = "";
}

void drawStatus() {
  uint32_t s = (millis() - sinceMs) / 1000;
  String sig = String(state) + reason + s + simMode + learnLeft;
  if (sig == sigS) return;
  sigS = sig;
  uint16_t c = ST_COL[state];
  M5.Display.fillRoundRect(20, 92, 600, 240, 10, c);
  ui::text(ST_FR[state], 320, 170, &fonts::FreeSansBold24pt7b, ui::WHITE, c, middle_center, 1.5f);
  ui::text("depuis " + dur(s), 320, 240, &fonts::FreeSansBold12pt7b, ui::WHITE, c, middle_center);
  String info = reason;
  if (simMode) info = "SIMULATION " + String(simMode == 1 ? "panne" : "arret");
  if (learnLeft) info = "Apprentissage... " + String(learnLeft) + " s";
  if (info.length()) ui::text(info, 320, 290, &fonts::FreeSans12pt7b, ui::WHITE, c, middle_center);
}

void drawGauge() {
  String sig = String(histI) + thOff + thHigh;
  if (sig == sigG) return;
  sigG = sig;
  int gx = 40, gy = 400, gw = 560, gh = 140;
  M5.Display.fillRect(gx - 10, gy - 10, gw + 20, gh + 50, ui::PANEL);
  float vmax = fmaxf(thHigh * 1.6f, 1.0f);
  for (int i = 0; i < 60; i++) if (hist[i] * 1.0f > vmax) vmax = hist[i];
  auto Y = [&](float v) { return gy + gh - (int)(v / vmax * gh); };
  M5.Display.fillRect(gx, Y(vmax), gw, Y(thHigh) - Y(vmax), ui::c565(250, 225, 225));
  M5.Display.drawFastHLine(gx, Y(thHigh), gw, ui::RED);
  M5.Display.drawFastHLine(gx, Y(thOff), gw, ui::GREY);
  M5.Display.drawRect(gx, gy, gw, gh, ui::LINE);
  int px = -1, py = 0;
  for (int i = 0; i < 60; i++) {
    float v = hist[(histI + i) % 60];
    int x = gx + i * gw / 59, y = Y(v);
    if (px >= 0) { M5.Display.drawLine(px, py, x, y, ui::BLUE); M5.Display.drawLine(px, py + 1, x, y + 1, ui::BLUE); }
    px = x; py = y;
  }
  char b[64];
  snprintf(b, sizeof b, "Activite %.1f   seuils %.1f / %.1f   irreg. %.2f", activity, thOff, thHigh, cv);
  ui::text(b, gx, gy + gh + 12, &fonts::FreeSans9pt7b, ui::INK, ui::PANEL);
}

void drawRight() {
  String sig = String(bDay) + bWeek + mtbf + mttr + prevDate + prevTask + curDate + curTask + abnBoot;
  if (sig == sigR) return;
  sigR = sig;
  int x = 660, y = 140;
  M5.Display.fillRect(650, 136, 600, 446, ui::PANEL);
  // two big counters
  const char* lab[2] = {"Pannes 24 h", "Pannes 7 jours"};
  int val[2] = {bDay, bWeek};
  for (int i = 0; i < 2; i++) {
    int bx = x + i * 295;
    M5.Display.fillRoundRect(bx, y, 280, 130, 10, ui::BG);
    ui::text(lab[i], bx + 16, y + 14, &fonts::FreeSans12pt7b, ui::MUTED, ui::BG);
    uint16_t c = val[i] > 0 ? ui::RED : ui::INK;
    ui::text(val[i] < 0 ? String("--") : String(val[i]), bx + 140, y + 82, &fonts::Font7, c, ui::BG, middle_center, 1.2f);
  }
  y += 150;
  char b[64];
  snprintf(b, sizeof b, "MTBF %s   MTTR %s   (anomalies depuis demarrage: %u)",
           mtbf < 0 ? "--" : (String(mtbf, 1) + " h").c_str(), mttr < 0 ? "--" : (String(mttr, 1) + " min").c_str(), (unsigned)abnBoot);
  ui::text(b, x, y, &fonts::FreeSans9pt7b, ui::MUTED, ui::PANEL);
  y += 36;
  // planned actions
  M5.Display.fillRoundRect(x, y, 575, 110, 10, ui::c565(225, 240, 250));
  ui::text("Prochaine action preventive", x + 16, y + 12, &fonts::FreeSans12pt7b, ui::BLUE, ui::c565(225, 240, 250));
  ui::text(prevDate.length() ? prevDate : String("non planifiee"), x + 16, y + 46, &fonts::FreeSansBold12pt7b, ui::INK, ui::c565(225, 240, 250));
  ui::text(prevTask, x + 16, y + 78, &fonts::FreeSans12pt7b, ui::INK, ui::c565(225, 240, 250));
  y += 126;
  M5.Display.fillRoundRect(x, y, 575, 110, 10, ui::c565(252, 236, 220));
  ui::text("Prochaine action curative", x + 16, y + 12, &fonts::FreeSans12pt7b, ui::c565(170, 90, 10), ui::c565(252, 236, 220));
  ui::text(curDate.length() ? curDate : String("aucune"), x + 16, y + 46, &fonts::FreeSansBold12pt7b, ui::INK, ui::c565(252, 236, 220));
  ui::text(curTask, x + 16, y + 78, &fonts::FreeSans12pt7b, ui::INK, ui::c565(252, 236, 220));
}

void drawAll() { drawStatic(); drawStatus(); drawGauge(); drawRight(); }

// ---------------------------------------------------------------- setup ----
void setup() {
  cp::initDisplay();
  prefs.begin("cplab", false);
  thOff = prefs.getFloat("thOff", TH_OFF_DEFAULT);
  thHigh = prefs.getFloat("thHigh", TH_HIGH_DEFAULT);

  pinMode(VIB_PIN, INPUT_PULLUP);
  lastLevel = digitalRead(VIB_PIN);
  if (VIB_AO_PIN >= 0) pinMode(VIB_AO_PIN, INPUT);
  esp_timer_create_args_t ta = {};
  ta.callback = sampleCb;
  ta.name = "vib";
  esp_timer_handle_t th;
  esp_timer_create(&ta, &th);
  esp_timer_start_periodic(th, 1000);           // 1 kHz
  sinceMs = millis();

  cp::onMessage = onMsg; cp::onSubscribe = onSub; cp::onAction = onAct;
  cp::begin(DEV, "maint", "maint", "Machine " + String(MACHINE_NUM) + " " MACHINE_NAME);
  drawAll();
}

void loop() {
  M5.update();
  cp::loop();

  static uint32_t lastEval = 0;
  if (millis() - lastEval >= 1000) { lastEval = millis(); evaluate(); }
  if (millis() - lastPub > 5000) publishState();

  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) {
    if (cp::overlayTouch()) {}
    else if (bAck.contains(t.x, t.y)) acknowledge();
    else if (bLearn.contains(t.x, t.y)) learnLeft = 10;
    else if (bFault.contains(t.x, t.y)) { simMode = 1; simUntil = millis() + 20000; }
    else if (bStop.contains(t.x, t.y))  { simMode = 2; simUntil = millis() + 20000; }
  }

  cp::service();
  if (cp::requestRedraw) { cp::requestRedraw = false; drawAll(); }
  static uint32_t lastUi = 0;
  if (cp::canDraw() && millis() - lastUi > 500) { lastUi = millis(); drawStatus(); drawGauge(); drawRight(); }
}
