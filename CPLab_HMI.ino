// ============================================================================
//  CPLab_HMI  -  Tab5 local operator panel (production)
//
//  - Line synoptic: 4 stations, counts, cadence, online state
//  - Kanban buffers between stations (WIP = count(n) - count(n+1) vs WIP max)
//  - OF progress by colour (cplab/prod/color/count vs cplab/prod/order)
//  - KPIs: output, total WIP, bottleneck (longest cycle) vs takt
//  - Andon buttons -> cplab/prod/andon {"from","type":"supervisor|quality"}
// ============================================================================
#include "cplab_common.h"

struct St {
  bool known = false, online = false;
  uint32_t count = 0, cycle = 0;
  float cadence = 0;
  String name, kanban = "UNKNOWN", state = "";
} st[4];

const char* COL_NAME[5] = {"RED", "GREEN", "BLUE", "BLACK", "UNKNOWN"};
const char* COL_FR[5]   = {"Rouge", "Vert", "Bleu", "Noir", "Inconnu"};
const uint16_t COL_UI[5] = {ui::RED, ui::GREEN, ui::BLUE, ui::INK, ui::GREY};
uint32_t colCnt[5] = {0}, target[4] = {0};
String ofName = "--";
float taktS = 0;
int wipMax = WIP_MAX_DEFAULT;
uint32_t andonUntil = 0;

const char* DEV_OF[4] = {"prod-st1", "prod-st2", "prod-st3", "prod-color"};

void onMsg(const String& t, const String& p) {
  String base = cp::topic("");
  String rel = t.substring(base.length());          // e.g. "prod/st2/count"
  if (rel.startsWith("status/")) {
    String id = rel.substring(7);
    for (int i = 0; i < 4; i++) if (id == DEV_OF[i]) st[i].online = (p == "online");
    return;
  }
  JsonDocument d;
  if (deserializeJson(d, p)) return;
  if (rel.startsWith("prod/st") && rel.endsWith("/count")) {
    int n = rel.charAt(7) - '1';
    if (n < 0 || n > 3) return;
    st[n].known = true;
    st[n].count = d["count"] | 0;
    st[n].cycle = d["cycle_ms"] | 0;
    st[n].cadence = d["cadence"] | 0.0f;
    st[n].name = (const char*)(d["name"] | "");
    st[n].kanban = (const char*)(d["kanban"] | "UNKNOWN");
    st[n].state = (const char*)(d["state"] | "");
  } else if (rel == "prod/color/count") {
    for (int i = 0; i < 5; i++) colCnt[i] = d[COL_NAME[i]] | 0;
  } else if (rel == "prod/order") {
    ofName = (const char*)(d["of"] | "--");
    taktS = d["takt_s"] | 0.0f;
    wipMax = d["wip_max"] | WIP_MAX_DEFAULT;
    for (int i = 0; i < 4; i++) target[i] = d["qty"][COL_NAME[i]] | 0;
    cp::requestRedraw = true;
  }
}
void onSub() {
  cp::subscribe("prod/+/count");
  cp::subscribe("prod/color/count");
  cp::subscribe("prod/order");
  cp::subscribe("status/+");
}

// ---------------------------------------------------------------- UI -------
const int SY = 92, SH = 230, SW = 236, GAP = 92, SX0 = 24;
ui::Button bAndonSup = {24, 612, 400, 88, "Appel superviseur", ui::AMBER};
ui::Button bAndonQual = {444, 612, 400, 88, "Alerte qualite", ui::RED};
String sigLine, sigOf, sigKpi;

uint16_t kanbanColor(int wip) {
  if (wip < 0) return ui::AMBER;
  if (wip > wipMax) return ui::RED;
  if (wip == wipMax) return ui::AMBER;
  return ui::GREEN;
}

void drawStatic() {
  M5.Display.fillScreen(ui::BG);
  cp::drawHeader(LINE_NAME, "OF " + ofName);
  ui::panel(24, 342, 760, 250, "Avancement OF par couleur");
  ui::panel(804, 342, 452, 250, "Indicateurs ligne");
  bAndonSup.draw(); bAndonQual.draw();
  sigLine = sigOf = sigKpi = "";
}

void drawLine() {
  String sig = String(wipMax);
  for (int i = 0; i < 4; i++) sig += String(st[i].count) + st[i].cadence + st[i].online + st[i].known + st[i].state + st[i].kanban;
  if (sig == sigLine) return;
  sigLine = sig;
  M5.Display.fillRect(0, SY, ui::W, SH + 10, ui::BG);
  // conveyor strip behind the stations
  M5.Display.fillRect(SX0, SY + SH / 2 - 6, 4 * SW + 3 * GAP, 12, ui::LINE);
  for (int i = 0; i < 4; i++) {
    int x = SX0 + i * (SW + GAP);
    uint16_t edge = !st[i].online ? ui::GREY : st[i].state == "RUN" ? ui::GREEN : ui::AMBER;
    M5.Display.fillRoundRect(x, SY, SW, SH, 10, ui::PANEL);
    M5.Display.fillRoundRect(x, SY, SW, 12, 6, edge);
    M5.Display.drawRoundRect(x, SY, SW, SH, 10, ui::LINE);
    ui::text("Poste " + String(i + 1), x + 14, SY + 22, &fonts::FreeSansBold12pt7b, ui::INK, ui::PANEL);
    ui::text(st[i].name, x + 14, SY + 50, &fonts::FreeSans9pt7b, ui::MUTED, ui::PANEL);
    ui::text(st[i].known ? String(st[i].count) : String("--"), x + SW / 2, SY + 125, &fonts::Font7, ui::INK, ui::PANEL, middle_center, 1.4f);
    char c[24];
    snprintf(c, sizeof c, "%.1f pcs/min", st[i].cadence);
    ui::text(c, x + SW / 2, SY + 190, &fonts::FreeSans12pt7b, ui::INK, ui::PANEL, middle_center);
    if (!st[i].online) ui::text("hors ligne", x + SW / 2, SY + 214, &fonts::FreeSans9pt7b, ui::RED, ui::PANEL, middle_center);
    // buffer to next station
    if (i < 3) {
      int bx = x + SW + GAP / 2, by = SY + SH / 2;
      if (st[i].known && st[i + 1].known) {
        int wip = (int)st[i].count - (int)st[i + 1].count;
        uint16_t kc = kanbanColor(wip);
        M5.Display.fillCircle(bx, by, 36, kc);
        ui::text(String(wip), bx, by + 2, &fonts::FreeSansBold18pt7b, ui::WHITE, kc, middle_center);
      } else {
        M5.Display.fillCircle(bx, by, 36, ui::GREY);
        ui::text("?", bx, by + 2, &fonts::FreeSansBold18pt7b, ui::WHITE, ui::GREY, middle_center);
      }
      ui::text("WIP", bx, by + 50, &fonts::FreeSans9pt7b, ui::MUTED, ui::BG, middle_center);
    }
  }
}

void drawOf() {
  String sig;
  for (int i = 0; i < 5; i++) sig += String(colCnt[i]) + ",";
  for (int i = 0; i < 4; i++) sig += String(target[i]) + ",";
  if (sig == sigOf) return;
  sigOf = sig;
  M5.Display.fillRect(34, 380, 740, 205, ui::PANEL);
  for (int i = 0; i < 4; i++) {
    int y = 386 + i * 48;
    M5.Display.fillRoundRect(44, y + 6, 30, 30, 5, COL_UI[i]);
    ui::text(COL_FR[i], 86, y + 21, &fonts::FreeSansBold12pt7b, ui::INK, ui::PANEL, middle_left);
    int bx = 200, bw = 420;
    M5.Display.fillRoundRect(bx, y + 13, bw, 16, 4, ui::BG);
    uint32_t den = target[i] ? target[i] : 1;
    int fw = (int)((uint64_t)bw * colCnt[i] / den);
    if (fw > bw) fw = bw;
    if (target[i] && fw > 0) M5.Display.fillRoundRect(bx, y + 13, fw, 16, 4, colCnt[i] > target[i] ? ui::AMBER : COL_UI[i]);
    String v = String(colCnt[i]) + (target[i] ? " / " + String(target[i]) : "");
    ui::text(v, 760, y + 21, &fonts::FreeSansBold12pt7b, ui::INK, ui::PANEL, middle_right);
  }
  if (colCnt[4]) ui::text("Inconnues: " + String(colCnt[4]), 44, 578, &fonts::FreeSans9pt7b, ui::RED, ui::PANEL, bottom_left);
}

void drawKpi() {
  int wipTot = 0, bott = -1; uint32_t maxCyc = 0;
  for (int i = 0; i < 3; i++) if (st[i].known && st[i + 1].known) wipTot += (int)st[i].count - (int)st[i + 1].count;
  for (int i = 0; i < 4; i++) if (st[i].cycle > maxCyc) { maxCyc = st[i].cycle; bott = i; }
  uint32_t tq = target[0] + target[1] + target[2] + target[3];
  String sig = String(wipTot) + bott + maxCyc + st[3].count + tq + taktS;
  if (sig == sigKpi) return;
  sigKpi = sig;
  M5.Display.fillRect(814, 380, 432, 205, ui::PANEL);
  int x = 824;
  ui::text("Sorties ligne: " + String(st[3].count) + (tq ? " / " + String(tq) : ""), x, 392, &fonts::FreeSansBold12pt7b, ui::INK, ui::PANEL);
  ui::text("En-cours total: " + String(wipTot), x, 432, &fonts::FreeSansBold12pt7b, ui::INK, ui::PANEL);
  char b[48];
  if (bott >= 0) snprintf(b, sizeof b, "Goulot: poste %d (%.1f s)", bott + 1, maxCyc / 1000.0f);
  else snprintf(b, sizeof b, "Goulot: --");
  bool over = taktS > 0 && maxCyc > taktS * 1000;
  ui::text(b, x, 472, &fonts::FreeSansBold12pt7b, over ? ui::RED : ui::INK, ui::PANEL);
  snprintf(b, sizeof b, "Takt: %.0f s", taktS);
  ui::text(taktS > 0 ? String(b) : String("Takt: --"), x, 512, &fonts::FreeSans12pt7b, ui::MUTED, ui::PANEL);
}

void drawAll() { drawStatic(); drawLine(); drawOf(); drawKpi(); }

void andon(const char* type) {
  JsonDocument d;
  d["from"] = "prod-hmi";
  d["type"] = type;
  cp::publishJson("prod/andon", d);
  M5.Speaker.tone(1500, 200);
  andonUntil = millis() + 1500;
}

void setup() {
  cp::initDisplay();
  cp::onMessage = onMsg;
  cp::onSubscribe = onSub;
  cp::begin("prod-hmi", "prod-hmi", "prod", "IHM ligne");
  drawAll();
}

void loop() {
  M5.update();
  cp::loop();
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) {
    if (cp::overlayTouch()) {}
    else if (bAndonSup.contains(t.x, t.y)) { andon("supervisor"); bAndonSup.draw(true); }
    else if (bAndonQual.contains(t.x, t.y)) { andon("quality"); bAndonQual.draw(true); }
  }
  if (andonUntil && millis() > andonUntil && cp::canDraw()) { andonUntil = 0; bAndonSup.draw(); bAndonQual.draw(); }

  cp::service();
  if (cp::requestRedraw) { cp::requestRedraw = false; drawAll(); }
  static uint32_t lastUi = 0;
  if (cp::canDraw() && millis() - lastUi > 300) { lastUi = millis(); drawLine(); drawOf(); drawKpi(); }
}
