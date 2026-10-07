// ============================================================================
//  CPLab_ColorStation  -  Tab5 end-of-line station (ST4) + colour control
//
//  - Hall sensor detects the piece -> after CAPTURE_DELAY_MS a frame is analysed
//  - average RGB of a central ROI -> nearest calibrated reference
//    (RED, GREEN, BLUE, BLACK, else UNKNOWN)
//  - publishes  cplab/prod/st4/count     (retained, same format as ST1..3)
//               cplab/prod/color/count   (retained) {"RED":n,...,"UNKNOWN":n,"total":n}
//               cplab/prod/color/last    {"color","r","g","b","dist"}
//  - listens    cplab/prod/st3/count, cplab/prod/order (target qty per colour)
//  - Calibration: press "Calibrer", place a piece under the camera, then press
//    the colour button -> reference saved in NVS.
//  - Without camera: colour buttons register a piece (simulation / test).
// ============================================================================
#include "cplab_common.h"
#include "tab5_camera.h"

const String KEY = "st4", PREV = "st3", DEV = "prod-color";

enum Col { C_RED, C_GREEN, C_BLUE, C_BLACK, C_UNKNOWN, C_N };
const char* COL_NAME[C_N]  = {"RED", "GREEN", "BLUE", "BLACK", "UNKNOWN"};
const char* COL_FR[C_N]    = {"Rouge", "Vert", "Bleu", "Noir", "Inconnu"};
const uint16_t COL_UI[C_N] = {ui::RED, ui::GREEN, ui::BLUE, ui::INK, ui::GREY};

// reference RGB (0..255) - overwritten by calibration
float ref[4][3] = {{180, 45, 45}, {45, 140, 70}, {40, 70, 175}, {28, 28, 30}};

uint32_t cnt[C_N] = {0}, total = 0, target[4] = {0}, prevCount = 0;
bool prevKnown = false;
String ofName = "--";
float taktS = TAKT_DEFAULT_S;
uint32_t cyc[6] = {0}; int cycN = 0, cycI = 0; uint32_t lastPieceMs = 0;

// live ROI measure
float roiR = 0, roiG = 0, roiB = 0;
int lastCol = -1; float lastDist = 0;
bool calibMode = false;

Preferences prefs;
bool saveDirty = false; uint32_t lastChange = 0, lastPub = 0;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t isrPending = 0, isrLast = 0;
void IRAM_ATTR hallISR() {
  uint32_t now = millis();
  portENTER_CRITICAL_ISR(&mux);
  if (now - isrLast >= MIN_PIECE_INTERVAL_MS) { isrLast = now; isrPending++; }
  portEXIT_CRITICAL_ISR(&mux);
}
uint32_t captureAt = 0; int captureQueue = 0;

// ---------------------------------------------------------------- vision ---
const int PW = 640, PH = 360;          // preview size on screen
uint16_t* preview = nullptr;

void analyseFrame(const uint16_t* px, uint32_t w, uint32_t h, bool withPreview) {
  // central square ROI
  uint32_t side = h * ROI_PERCENT / 100, x0 = (w - side) / 2, y0 = (h - side) / 2;
  uint64_t sr = 0, sg = 0, sb = 0; uint32_t n = 0;
  for (uint32_t y = y0; y < y0 + side; y += 4)
    for (uint32_t x = x0; x < x0 + side; x += 4) {
      uint16_t p = px[y * w + x];
      sr += ((p >> 11) & 0x1F) << 3;
      sg += ((p >> 5) & 0x3F) << 2;
      sb += (p & 0x1F) << 3;
      n++;
    }
  if (n) { roiR = sr / (float)n; roiG = sg / (float)n; roiB = sb / (float)n; }
  if (withPreview && preview) {
    for (int y = 0; y < PH; y++) {
      uint32_t sy = (uint32_t)y * h / PH;
      for (int x = 0; x < PW; x++) preview[y * PW + x] = px[sy * w + (uint32_t)x * w / PW];
    }
  }
}

// distance in (chromaticity + brightness) space, robust to lighting changes
float colorDist(float r, float g, float b, const float* rf) {
  float s1 = r + g + b + 1, s2 = rf[0] + rf[1] + rf[2] + 1;
  float dr = r / s1 - rf[0] / s2, dg = g / s1 - rf[1] / s2, db = b / s1 - rf[2] / s2;
  float dv = (s1 - s2) / 765.0f;
  return sqrtf(dr * dr + dg * dg + db * db + 0.5f * dv * dv);
}
int classify(float r, float g, float b, float& best) {
  int bi = C_UNKNOWN; best = 9;
  for (int i = 0; i < 4; i++) {
    float d = colorDist(r, g, b, ref[i]);
    if (d < best) { best = d; bi = i; }
  }
  return best <= UNKNOWN_DIST ? bi : C_UNKNOWN;
}

// ---------------------------------------------------------------- data -----
uint32_t avgCycle() { if (!cycN) return 0; uint32_t s = 0; for (int i = 0; i < cycN; i++) s += cyc[i]; return s / cycN; }

void publishAll() {
  JsonDocument d;
  uint32_t a = avgCycle();
  d["st"] = 4; d["id"] = KEY; d["name"] = STATION_NAME;
  d["count"] = total; d["cycle_ms"] = a;
  d["cadence"] = a ? roundf(600000.0f / a) / 10.0f : 0;
  d["takt_s"] = taktS;
  d["wip_out"] = -1;
  bool done = false; uint32_t tq = target[0] + target[1] + target[2] + target[3];
  if (tq) done = total >= tq;
  d["kanban"] = done ? "DONE" : "GREEN";
  d["state"] = (lastPieceMs == 0 || millis() - lastPieceMs > 3 * taktS * 1000) ? "WAIT" : "RUN";
  cp::publishJson("prod/" + KEY + "/count", d, true);

  JsonDocument c;
  for (int i = 0; i < C_N; i++) c[COL_NAME[i]] = cnt[i];
  c["total"] = total;
  cp::publishJson("prod/color/count", c, true);
  lastPub = millis();
}

void registerPiece(int col) {
  uint32_t now = millis();
  if (lastPieceMs) { cyc[cycI] = now - lastPieceMs; cycI = (cycI + 1) % 6; if (cycN < 6) cycN++; }
  lastPieceMs = now;
  cnt[col]++; total++; lastCol = col;
  JsonDocument l;
  l["color"] = COL_NAME[col]; l["r"] = (int)roiR; l["g"] = (int)roiG; l["b"] = (int)roiB;
  l["dist"] = roundf(lastDist * 1000) / 1000.0f;
  cp::publishJson("prod/color/last", l);
  saveDirty = true; lastChange = now;
  publishAll();
}

void resetCounts() {
  for (int i = 0; i < C_N; i++) cnt[i] = 0;
  total = 0; cycN = cycI = 0; lastPieceMs = 0; lastCol = -1;
  saveDirty = true; lastChange = millis();
  publishAll();
}

void saveNvs() {
  prefs.putBytes("cnt", cnt, sizeof cnt);
  prefs.putUInt("total", total);
}

void onMsg(const String& t, const String& p) {
  JsonDocument d;
  if (deserializeJson(d, p)) return;
  if (t == cp::topic("prod/" + PREV + "/count")) { prevCount = d["count"] | 0; prevKnown = true; }
  else if (t == cp::topic("prod/order")) {
    ofName = d["of"] | "--";
    taktS = d["takt_s"] | TAKT_DEFAULT_S;
    for (int i = 0; i < 4; i++) target[i] = d["qty"][COL_NAME[i]] | 0;
    cp::requestRedraw = true;
  }
}
void onSub() { cp::subscribe("prod/" + PREV + "/count"); cp::subscribe("prod/order"); publishAll(); }
void onAct(JsonDocument& d) {
  String c = d["cmd"] | "";
  if (c == "reset") resetCounts();
  else if (c == "sim_piece") {
    String col = d["color"] | "RED";
    for (int i = 0; i < C_N; i++) if (col == COL_NAME[i]) registerPiece(i);
  }
}

// ---------------------------------------------------------------- UI -------
const int VX = 20, VY = 92;                 // preview 640x360
const int RX = 680, RY = 92, RW = 580, RH = 500;
ui::Button bCol[4] = {{20, 612, 150, 88, "Rouge", ui::RED}, {180, 612, 150, 88, "Vert", ui::GREEN},
                      {340, 612, 150, 88, "Bleu", ui::BLUE}, {500, 612, 150, 88, "Noir", ui::INK}};
ui::Button bCal   = {680, 612, 270, 88, "Calibrer", ui::MUTED};
ui::Button bReset = {970, 612, 290, 88, "Reset (maintenir)", ui::GREY};
String sigR, sigM;

void drawStatic() {
  M5.Display.fillScreen(ui::BG);
  cp::drawHeader("Poste 4 - " STATION_NAME, "OF " + ofName);
  ui::panel(RX, RY, RW, RH, "Production par couleur / objectif OF");
  for (auto& b : bCol) b.draw();
  bCal.draw(calibMode);
  bReset.draw();
  if (!cam::ok) {
    ui::panel(VX, VY, PW, PH);
    ui::text("Camera indisponible", VX + PW / 2, VY + PH / 2 - 20, &fonts::FreeSansBold18pt7b, ui::INK, ui::PANEL, middle_center);
    ui::text(cam::err + " - mode simulation (boutons)", VX + PW / 2, VY + PH / 2 + 24, &fonts::FreeSans12pt7b, ui::MUTED, ui::PANEL, middle_center);
  }
  sigR = sigM = "";
}

void drawPreview() {
  if (!cam::ok || !preview) return;
  M5.Display.pushImage(VX, VY, PW, PH, preview);
  int side = PH * ROI_PERCENT / 100;
  M5.Display.drawRect(VX + (PW - side) / 2, VY + (PH - side) / 2, side, side, ui::WHITE);
  M5.Display.drawRect(VX + (PW - side) / 2 - 1, VY + (PH - side) / 2 - 1, side + 2, side + 2, ui::INK);
}

void drawMeasure() {   // below the preview: live RGB + last detection
  char s[48];
  snprintf(s, sizeof s, "R %3d  G %3d  B %3d", (int)roiR, (int)roiG, (int)roiB);
  String sig = String(s) + lastCol + calibMode;
  if (sig == sigM) return;
  sigM = sig;
  int y = VY + PH + 10;
  M5.Display.fillRect(VX, y, PW, 130, ui::BG);
  ui::panel(VX, y, PW, 130);
  M5.Display.fillRoundRect(VX + 16, y + 16, 98, 98, 8, ui::c565(roiR, roiG, roiB));
  ui::text(calibMode ? "CALIBRATION: choisir la couleur" : "Mesure zone centrale",
           VX + 130, y + 22, &fonts::FreeSans12pt7b, calibMode ? ui::AMBER : ui::MUTED, ui::PANEL);
  ui::text(s, VX + 130, y + 56, &fonts::FreeSansBold12pt7b, ui::INK, ui::PANEL);
  if (lastCol >= 0) {
    M5.Display.fillRoundRect(VX + 430, y + 16, 190, 98, 8, COL_UI[lastCol]);
    ui::text(COL_FR[lastCol], VX + 525, y + 65, &fonts::FreeSansBold18pt7b, ui::WHITE, COL_UI[lastCol], middle_center);
  }
}

void drawRight() {
  String sig = String(total) + prevCount + prevKnown;
  for (int i = 0; i < C_N; i++) sig += "," + String(cnt[i]);
  for (int i = 0; i < 4; i++) sig += "," + String(target[i]);
  if (sig == sigR) return;
  sigR = sig;
  M5.Display.fillRect(RX + 10, RY + 46, RW - 20, RH - 56, ui::PANEL);
  for (int i = 0; i < C_N; i++) {
    int y = RY + 56 + i * 64;
    M5.Display.fillRoundRect(RX + 20, y + 8, 36, 36, 6, COL_UI[i]);
    ui::text(COL_FR[i], RX + 70, y + 26, &fonts::FreeSansBold12pt7b, ui::INK, ui::PANEL, middle_left);
    String v = String(cnt[i]) + (i < 4 && target[i] ? " / " + String(target[i]) : "");
    ui::text(v, RX + RW - 24, y + 26, &fonts::FreeSansBold12pt7b, ui::INK, ui::PANEL, middle_right);
    int bx = RX + 190, bw = RW - 330;
    M5.Display.fillRoundRect(bx, y + 18, bw, 16, 4, ui::BG);
    uint32_t den = (i < 4 && target[i]) ? target[i] : (total ? total : 1);
    int fw = (int)((uint64_t)bw * cnt[i] / den); if (fw > bw) fw = bw;
    uint16_t bc = (i < 4 && target[i] && cnt[i] > target[i]) ? ui::AMBER : COL_UI[i];
    if (fw > 0) M5.Display.fillRoundRect(bx, y + 18, fw, 16, 4, bc);
  }
  int y = RY + 56 + C_N * 64 + 6;
  M5.Display.drawFastHLine(RX + 20, y, RW - 40, ui::LINE);
  ui::text("Total sorti: " + String(total), RX + 20, y + 16, &fonts::FreeSansBold18pt7b, ui::INK, ui::PANEL);
  String wip = prevKnown ? String((int)prevCount - (int)total) : String("?");
  ui::text("En-cours depuis poste 3: " + wip, RX + 20, y + 62, &fonts::FreeSans12pt7b, ui::INK, ui::PANEL);
}

void drawAll() { drawStatic(); drawPreview(); drawMeasure(); drawRight(); }

// ---------------------------------------------------------------- setup ----
void setup() {
  cp::initDisplay();
  prefs.begin("cplab", false);
  prefs.getBytes("cnt", cnt, sizeof cnt);
  total = prefs.getUInt("total", 0);
  if (prefs.isKey("ref")) prefs.getBytes("ref", ref, sizeof ref);

  preview = (uint16_t*)heap_caps_malloc(PW * PH * 2, MALLOC_CAP_SPIRAM);
  // M5.Display.setSwapBytes(true);   // uncomment if preview colours look wrong
  cam::begin();

  pinMode(HALL_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(HALL_PIN), hallISR, HALL_ACTIVE_LOW ? FALLING : RISING);

  cp::onMessage = onMsg; cp::onSubscribe = onSub; cp::onAction = onAct;
  cp::begin(DEV, "prod-color", "prod", "Poste 4 couleur");
  drawAll();
}

void loop() {
  M5.update();
  cp::loop();

  uint32_t pending;
  portENTER_CRITICAL(&mux); pending = isrPending; isrPending = 0; portEXIT_CRITICAL(&mux);
  if (pending) { captureQueue += pending; if (!captureAt) captureAt = millis() + CAPTURE_DELAY_MS; }

  // capture & classify on Hall trigger
  if (captureQueue && captureAt && millis() >= captureAt) {
    if (cam::ok) cam::grab([](const uint16_t* p, uint32_t w, uint32_t h) { analyseFrame(p, w, h, false); });
    int c = cam::ok ? classify(roiR, roiG, roiB, lastDist) : C_UNKNOWN;
    registerPiece(c);
    captureQueue--;
    captureAt = captureQueue ? millis() + CAPTURE_DELAY_MS : 0;
  }

  // live preview ~5 fps
  static uint32_t lastPrev = 0;
  if (cam::ok && cp::canDraw() && millis() - lastPrev > 200) {
    lastPrev = millis();
    cam::grab([](const uint16_t* p, uint32_t w, uint32_t h) { analyseFrame(p, w, h, true); });
    drawPreview();
  }

  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) {
    if (cp::overlayTouch()) {}
    else if (bCal.contains(t.x, t.y)) { calibMode = !calibMode; bCal.draw(calibMode); sigM = ""; }
    else for (int i = 0; i < 4; i++) if (bCol[i].contains(t.x, t.y)) {
      if (calibMode && cam::ok) {
        ref[i][0] = roiR; ref[i][1] = roiG; ref[i][2] = roiB;
        prefs.putBytes("ref", ref, sizeof ref);
        calibMode = false; bCal.draw(false); sigM = "";
        M5.Speaker.tone(2200, 100);
      } else { lastDist = 0; registerPiece(i); }       // test / simulation
    }
  }
  if (t.wasHold() && bReset.contains(t.x, t.y) && !cp::ov.visible) resetCounts();

  if (millis() - lastPub > 5000) publishAll();
  if (saveDirty && millis() - lastChange > 3000) { saveNvs(); saveDirty = false; }

  cp::service();
  if (cp::requestRedraw) { cp::requestRedraw = false; drawAll(); }
  static uint32_t lastUi = 0;
  if (cp::canDraw() && millis() - lastUi > 250) { lastUi = millis(); drawMeasure(); drawRight(); }
}
