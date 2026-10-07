// ============================================================================
//  CPLab_ProdStation  -  Tab5 production station (ST1, ST2, ST3)
//
//  - counts pieces with a Hall sensor (interrupt + debounce, saved in NVS)
//  - computes cycle time, cadence (pcs/min) and compares to the takt time
//  - Kanban: WIP in the buffer to the next station = myCount - nextCount
//      GREEN  "PRODUIRE"         WIP <  WIP max
//      AMBER  "LIMITE ATTEINTE"  WIP == WIP max
//      RED    "STOP KANBAN"      WIP >  WIP max  (overproduction)
//  - publishes  cplab/prod/stN/count  (retained JSON)
//  - listens    cplab/prod/st(N+1)/count, cplab/prod/st(N-1)/count, cplab/prod/order
//  - remote:    cplab/cmd/prod-stN/action  {"cmd":"reset"|"set","value":n|"sim_piece"}
// ============================================================================
#include "cplab_common.h"

// Types used as return/parameter types must be declared BEFORE the first
// function: the Arduino IDE inserts automatic prototypes there.
struct Kanban { const char* code; const char* label; uint16_t color; };

const int ST = STATION_NUM;
const String KEY  = "st" + String(ST);
const String NEXT = "st" + String(ST + 1);
const String PREV = (ST > 1) ? String("st") + String(ST - 1) : String("");
const String DEV  = "prod-" + KEY;

// ---------------------------------------------------------------- counting --
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t isrPending = 0, isrLast = 0;
void IRAM_ATTR hallISR() {
  uint32_t now = millis();
  portENTER_CRITICAL_ISR(&mux);
  if (now - isrLast >= MIN_PIECE_INTERVAL_MS) { isrLast = now; isrPending++; }
  portEXIT_CRITICAL_ISR(&mux);
}

uint32_t count = 0, nextCount = 0, prevCount = 0;
bool nextKnown = false, prevKnown = (ST == 1);
uint32_t cyc[6] = {0};
int cycN = 0, cycI = 0;
uint32_t lastPieceMs = 0;

// order (from cplab/prod/order)
String ofName = "--";
uint32_t orderQty = 0;
float taktS = TAKT_DEFAULT_S;
int wipMax = WIP_MAX_DEFAULT;

Preferences prefs;
bool saveDirty = false;
uint32_t lastChange = 0, lastPub = 0;

uint32_t avgCycle() {
  if (!cycN) return 0;
  uint32_t s = 0;
  for (int i = 0; i < cycN; i++) s += cyc[i];
  return s / cycN;
}
float cadence() { uint32_t a = avgCycle(); return a ? 60000.0f / a : 0; }
bool waiting() { return lastPieceMs == 0 || millis() - lastPieceMs > (uint32_t)(3 * taktS * 1000); }
int wipOut() { return nextKnown ? (int)count - (int)nextCount : 0; }

// Kanban decision -----------------------------------------------------------
Kanban kanban() {
  if (ST == 1 && orderQty > 0 && count >= orderQty) return {"DONE", "OF TERMINE", ui::BLUE};
  if (!nextKnown) return {"UNKNOWN", "POSTE SUIVANT ?", ui::GREY};
  int w = wipOut();
  if (w < 0)       return {"ERROR",  "ECART COMPTAGE",  ui::AMBER};
  if (w > wipMax)  return {"RED",    "STOP KANBAN",     ui::RED};
  if (w == wipMax) return {"AMBER",  "LIMITE ATTEINTE", ui::AMBER};
  return {"GREEN", "PRODUIRE", ui::GREEN};
}
// Takt comparison
uint16_t taktColor() {
  uint32_t a = avgCycle();
  if (!a) return ui::GREY;
  float r = a / (taktS * 1000.0f);
  return r <= 1.0f ? ui::GREEN : r <= 1.15f ? ui::AMBER : ui::RED;
}

void publishCount() {
  JsonDocument d;
  Kanban k = kanban();
  d["st"] = ST;
  d["id"] = KEY;
  d["name"] = STATION_NAME;
  d["count"] = count;
  d["cycle_ms"] = avgCycle();
  d["cadence"] = roundf(cadence() * 10) / 10.0f;
  d["takt_s"] = taktS;
  d["wip_out"] = nextKnown ? wipOut() : -1;
  d["wip_max"] = wipMax;
  d["kanban"] = k.code;
  d["state"] = waiting() ? "WAIT" : "RUN";
  cp::publishJson("prod/" + KEY + "/count", d, true);
  lastPub = millis();
}

void registerPiece() {
  uint32_t now = millis();
  if (lastPieceMs) {
    cyc[cycI] = now - lastPieceMs;
    cycI = (cycI + 1) % 6;
    if (cycN < 6) cycN++;
  }
  lastPieceMs = now;
  count++;
  saveDirty = true;
  lastChange = now;
  publishCount();
}

void resetCount(uint32_t v = 0) {
  count = v; cycN = cycI = 0; lastPieceMs = 0;
  saveDirty = true; lastChange = millis();
  publishCount();
}

// ---------------------------------------------------------------- MQTT -----
void onMsg(const String& t, const String& p) {
  JsonDocument d;
  if (deserializeJson(d, p)) return;
  if (t == cp::topic("prod/" + NEXT + "/count")) { nextCount = d["count"] | 0; nextKnown = true; }
  else if (PREV.length() && t == cp::topic("prod/" + PREV + "/count")) { prevCount = d["count"] | 0; prevKnown = true; }
  else if (t == cp::topic("prod/order")) {
    ofName = d["of"] | "--";
    taktS  = d["takt_s"] | TAKT_DEFAULT_S;
    wipMax = d["wip_max"] | WIP_MAX_DEFAULT;
    orderQty = 0;
    if (d["qty"].is<JsonObject>()) for (JsonPair kv : d["qty"].as<JsonObject>()) orderQty += kv.value().as<uint32_t>();
    cp::requestRedraw = true;   // header shows the OF number
  }
}
void onSub() {
  cp::subscribe("prod/" + NEXT + "/count");
  if (PREV.length()) cp::subscribe("prod/" + PREV + "/count");
  cp::subscribe("prod/order");
  publishCount();
}
void onAct(JsonDocument& d) {
  String c = d["cmd"] | "";
  if (c == "reset") resetCount(0);
  else if (c == "set") resetCount(d["value"] | 0);
  else if (c == "sim_piece") registerPiece();
}

// ---------------------------------------------------------------- UI -------
const int AX = 20, AY = 92, AW = 600, AH = 500;     // left panel: counting
const int BX = 640, BY = 92, BW = 620, BH = 500;    // right panel: kanban
ui::Button bTest  = {20, 612, 300, 88, "+1 piece (test)", ui::BLUE};
ui::Button bReset = {340, 612, 300, 88, "Reset (maintenir)", ui::GREY};
ui::Button bInfo  = {660, 612, 300, 88, "Identifier", ui::MUTED};
String sigA, sigB;

void drawStatic() {
  M5.Display.fillScreen(ui::BG);
  cp::drawHeader("Poste " + String(ST) + " - " + STATION_NAME, "OF " + ofName);
  ui::panel(AX, AY, AW, AH, "Pieces comptees");
  ui::panel(BX, BY, BW, BH, ("Kanban vers poste " + String(ST + 1)).c_str());
  bTest.draw(); bReset.draw(); bInfo.draw();
  sigA = sigB = "";
}

void drawA() {
  char cyc_s[16], tk_s[16], cad_s[16];
  snprintf(cad_s, sizeof cad_s, "%.1f", cadence());
  snprintf(cyc_s, sizeof cyc_s, "%.1f s", avgCycle() / 1000.0f);
  snprintf(tk_s, sizeof tk_s, "%.0f s", taktS);
  String sig = String(count) + cad_s + cyc_s + tk_s + waiting();
  if (sig == sigA) return;
  sigA = sig;
  M5.Display.fillRect(AX + 10, AY + 50, AW - 20, AH - 60, ui::PANEL);
  ui::text(String(count), AX + AW / 2, AY + 150, &fonts::Font7, ui::INK, ui::PANEL, middle_center, 3.0f);
  ui::text("Cadence", AX + 30, AY + 270, &fonts::FreeSans12pt7b, ui::MUTED, ui::PANEL);
  ui::text(String(cad_s) + " pcs/min", AX + 30, AY + 300, &fonts::FreeSansBold18pt7b, ui::INK, ui::PANEL);
  ui::text("Temps de cycle / takt", AX + 30, AY + 360, &fonts::FreeSans12pt7b, ui::MUTED, ui::PANEL);
  uint16_t tc = taktColor();
  M5.Display.fillRoundRect(AX + 30, AY + 390, 260, 56, 8, tc);
  ui::text(avgCycle() ? String(cyc_s) : String("--"), AX + 160, AY + 418, &fonts::FreeSansBold18pt7b, ui::WHITE, tc, middle_center);
  ui::text("takt " + String(tk_s), AX + 310, AY + 418, &fonts::FreeSansBold18pt7b, ui::INK, ui::PANEL, middle_left);
  uint16_t sc = waiting() ? ui::AMBER : ui::GREEN;
  M5.Display.fillCircle(AX + 44, AY + 470, 9, sc);
  ui::text(waiting() ? "En attente de pieces" : "En production", AX + 62, AY + 470, &fonts::FreeSans12pt7b, ui::INK, ui::PANEL, middle_left);
}

void drawB() {
  Kanban k = kanban();
  int w = wipOut();
  String sig = String(k.code) + w + wipMax + nextCount + prevCount + nextKnown + orderQty + count;
  if (sig == sigB) return;
  sigB = sig;
  M5.Display.fillRect(BX + 10, BY + 50, BW - 20, BH - 60, ui::PANEL);

  // buffer slots (WIP max cards)
  int slots = constrain(wipMax, 1, 8);
  int sw = 62, gap = 10, sx = BX + 30, sy = BY + 60;
  for (int i = 0; i < slots; i++) {
    bool full = nextKnown && i < w;
    M5.Display.fillRoundRect(sx + i * (sw + gap), sy, sw, 80, 6, full ? k.color : ui::BG);
    M5.Display.drawRoundRect(sx + i * (sw + gap), sy, sw, 80, 6, ui::LINE);
  }
  if (nextKnown && w > slots)
    ui::text("+" + String(w - slots), sx + slots * (sw + gap) + 8, sy + 40, &fonts::FreeSansBold18pt7b, ui::RED, ui::PANEL, middle_left);
  ui::text("En-cours (WIP): " + (nextKnown ? String(w) : String("?")) + " / max " + String(wipMax),
           BX + 30, sy + 100, &fonts::FreeSansBold12pt7b, ui::INK, ui::PANEL);

  // instruction tile
  M5.Display.fillRoundRect(BX + 30, BY + 210, BW - 60, 120, 12, k.color);
  ui::text(k.label, BX + BW / 2, BY + 270, &fonts::FreeSansBold24pt7b, ui::WHITE, k.color, middle_center);

  // expected vs received at next station
  ui::text("Attendu au poste " + String(ST + 1) + ": " + String(count) + "   recu: " + (nextKnown ? String(nextCount) : String("?")),
           BX + 30, BY + 360, &fonts::FreeSans12pt7b, ui::INK, ui::PANEL);
  String up;
  if (ST == 1) up = orderQty ? "OF: " + String(count) + " / " + String(orderQty) + " lances" : "Aucun OF recu";
  else up = "Amont (poste " + String(ST - 1) + "): " + (prevKnown ? String((int)prevCount - (int)count) : String("?")) + " piece(s) en attente";
  ui::text(up, BX + 30, BY + 400, &fonts::FreeSans12pt7b, ui::INK, ui::PANEL);
}

void drawAll() { drawStatic(); drawA(); drawB(); }

// ---------------------------------------------------------------- setup ----
void setup() {
  cp::initDisplay();
  prefs.begin("cplab", false);
  count = prefs.getUInt("count", 0);

  pinMode(HALL_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(HALL_PIN), hallISR, HALL_ACTIVE_LOW ? FALLING : RISING);

  cp::onMessage = onMsg;
  cp::onSubscribe = onSub;
  cp::onAction = onAct;
  cp::begin(DEV, "prod-station", "prod", String("Poste ") + ST + " " + STATION_NAME);
  drawAll();
}

void loop() {
  M5.update();
  cp::loop();

  uint32_t pending;
  portENTER_CRITICAL(&mux);
  pending = isrPending; isrPending = 0;
  portEXIT_CRITICAL(&mux);
  while (pending--) registerPiece();

  auto t = M5.Touch.getDetail();
  if (t.wasPressed() && !cp::ov.visible) {
    if (bTest.contains(t.x, t.y)) registerPiece();
    else if (bInfo.contains(t.x, t.y)) cp::identifyUntil = millis() + 3000;
  } else if (t.wasPressed()) cp::overlayTouch();
  if (t.wasHold() && bReset.contains(t.x, t.y) && !cp::ov.visible) resetCount(0);

  if (millis() - lastPub > 5000) publishCount();      // refresh RUN/WAIT state
  if (saveDirty && millis() - lastChange > 3000) { prefs.putUInt("count", count); saveDirty = false; }

  cp::service();
  if (cp::requestRedraw) { cp::requestRedraw = false; drawAll(); }
  static uint32_t lastUi = 0;
  if (cp::canDraw() && millis() - lastUi > 250) { lastUi = millis(); drawA(); drawB(); }
}
