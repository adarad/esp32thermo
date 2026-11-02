// Zakladka: strona "Swiatlo" - wlacznik, jasnosc, barwa (cieple <-> zimne LED), 3 presety.
// Sterowanie bezposrednie przez MQTT (patrz lamp_config.h). Przelaczanie stron: przesuniecie palcem
// w lewo/prawo albo dotkniecie wskaznika stron (kropki) na dole.
//
// Korzysta z globalnych zmiennych z glownego pliku: page, lampOn, lampBright, lampColor, lampDirty ...
// i funkcji z ha_mqtt.ino: lampSendPower(), lampSendBrightness(), lampSendColor().

#include "lamp_config.h"

// ---------- uklad ----------
static const int PW_X = 240, PW_Y = 128, PW_R = 58;          // przycisk wlacz/wylacz
static const int SL_X0 = 60, SL_X1 = 420, SL_R = 15;         // suwaki: zakres X, promien toru
static const int SL_BR_Y = 272, SL_CT_Y = 352;               // srodki suwakow (jasnosc, barwa)
static const int SL_HIT = 34;                                // polowa wysokosci strefy dotyku
static const int CHIP_Y = 396, CHIP_H = 44, CHIP_W = 128;    // presety
static const int CHIP_X[3] = {24, 176, 328};

static const uint16_t COL_WARM = TC(255, 190, 90);

static const char *PRESET_NAME[3] = {"CZYTANIE", "WIECZOR", "NOC"};
static const int PRESET_BR[3]     = {100, 45, 8};
static const int PRESET_CT[3]     = {70, 5, 0};

// ---------- stan wewnetrzny strony ----------
static bool lightDown = false;
static int activeSlider = 0;      // 0 brak, 1 jasnosc, 2 barwa
static int pressedItem = 0;       // 0 brak, 1 power, 2..4 presety
static int lastBrKnobX = -1, lastCtKnobX = -1;
static int lastSentBr = -1, lastSentCt = -1;
static uint32_t lastSendMs = 0;

// ---------- pomocnicze ----------
uint16_t lampAccent() {
  return lampOn ? COL_WARM : COL_TRACK2;
}

uint16_t ctColor(int pct) {   // cieple (255,170,70) -> zimne (165,205,255)
  int r = 255 + (165 - 255) * pct / 100;
  int g = 170 + (205 - 170) * pct / 100;
  int b = 70 + (255 - 70) * pct / 100;
  return TC(r, g, b);
}

int knobX(int val) {
  return SL_X0 + (SL_X1 - SL_X0) * val / 100;
}

// ---------- rysowanie ----------
void lightDrawTopBar() {
  gfx->fillRect(0, 0, W, 58, COL_BG);
  gfx->setTextSize(3);
  gfx->setTextColor(COL_TEXT, COL_BG);
  gfx->setCursor(24, 17);
  gfx->print(LAMP_NAME);

  gfx->setTextSize(2);
  gfx->setTextColor(netOnline ? COL_OK : COL_TRACK2, COL_BG);
  gfx->setCursor(24 + (int)strlen(LAMP_NAME) * 18 + 14, 22);
  gfx->print("HA");

  const char *txt;
  uint16_t col;
  if (!netOnline)      { txt = "BRAK SIECI"; col = COL_ERR; }
  else if (lampOn)     { txt = "WLACZONE";   col = COL_WARM; }
  else                 { txt = "WYLACZONE";  col = COL_MUTED; }

  int w = strlen(txt) * 6 * 2;
  int x = W - 24 - w;
  gfx->fillCircle(x - 16, 29, 6, col);
  gfx->setTextSize(2);
  gfx->setTextColor(col, COL_BG);
  gfx->setCursor(x, 22);
  gfx->print(txt);

  gfx->drawFastHLine(24, 57, W - 48, COL_TRACK);
}

void drawPower(bool pressed) {
  uint16_t acc = lampAccent();
  gfx->fillCircle(PW_X, PW_Y, PW_R + 3, COL_BG);   // wyczysc stare obramowanie
  gfx->fillCircle(PW_X, PW_Y, PW_R, pressed ? COL_CARD_P : COL_CARD);
  int rings = lampOn ? 3 : 1;
  for (int i = 0; i < rings; i++) gfx->drawCircle(PW_X, PW_Y, PW_R - i, lampOn ? acc : COL_TRACK2);

  uint16_t ic = lampOn ? acc : COL_MUTED;
  for (int d = 305; d <= 595; d += 4) {             // luk ikony zasilania, przerwa u gory
    float a = d * DEG_TO_RAD;
    gfx->fillCircle(PW_X + (int)lroundf(cosf(a) * 26), PW_Y + (int)lroundf(sinf(a) * 26), 4, ic);
  }
  gfx->fillRoundRect(PW_X - 3, PW_Y - 36, 6, 34, 3, ic);
}

void drawPowerLabel() {
  gfx->fillRect(100, 194, 280, 18, COL_BG);
  drawCentered(lampOn ? "WLACZONE" : "WYLACZONE", CX, 196, 2, lampOn ? COL_WARM : COL_MUTED, COL_BG);
}

void drawSliderLabel(int y, const char *name, const char *val) {
  gfx->fillRect(SL_X0, y, SL_X1 - SL_X0, 16, COL_BG);
  gfx->setTextSize(2);
  gfx->setTextColor(COL_MUTED, COL_BG);
  gfx->setCursor(SL_X0, y);
  gfx->print(name);
  int w = strlen(val) * 12;
  gfx->setTextColor(COL_TEXT, COL_BG);
  gfx->setCursor(SL_X1 - w, y);
  gfx->print(val);
}

void drawBrightness() {
  int cy = SL_BR_Y;
  uint16_t acc = lampAccent();
  gfx->fillRect(SL_X0 - 24, cy - 24, SL_X1 - SL_X0 + 48, 48, COL_BG);
  gfx->fillRoundRect(SL_X0, cy - SL_R, SL_X1 - SL_X0, 2 * SL_R, SL_R, COL_TRACK);

  int x = knobX(lampBright);
  int fw = x - SL_X0;
  if (fw >= SL_R) {
    gfx->fillCircle(SL_X0 + SL_R, cy, SL_R, acc);
    gfx->fillRect(SL_X0 + SL_R, cy - SL_R, fw - SL_R, 2 * SL_R, acc);
  }
  gfx->fillCircle(x, cy, 19, COL_TEXT);
  gfx->fillCircle(x, cy, 10, acc);
  lastBrKnobX = x;

  char b[8];
  snprintf(b, sizeof(b), "%d%%", lampBright);
  drawSliderLabel(232, "JASNOSC", b);
}

int trackHalfH(int x) {       // polowa wysokosci toru w kolumnie x (zaokraglone konce)
  int dx = 0;
  if (x < SL_X0 + SL_R) dx = SL_X0 + SL_R - x;
  else if (x > SL_X1 - SL_R) dx = x - (SL_X1 - SL_R);
  if (dx <= 0) return SL_R;
  if (dx > SL_R) return 0;
  return (int)sqrtf((float)(SL_R * SL_R - dx * dx));
}

void drawColorTrackRange(int xa, int xb) {
  if (xa < SL_X0) xa = SL_X0;
  if (xb > SL_X1) xb = SL_X1;
  for (int x = xa; x <= xb; x++) {
    int hh = trackHalfH(x);
    if (hh <= 0) continue;
    int pct = (x - SL_X0) * 100 / (SL_X1 - SL_X0);
    gfx->drawFastVLine(x, SL_CT_Y - hh, 2 * hh + 1, ctColor(pct));
  }
}

void drawColorSlider() {
  int cy = SL_CT_Y;
  int x = knobX(lampColor);
  if (lastCtKnobX < 0) {
    gfx->fillRect(SL_X0 - 24, cy - 24, SL_X1 - SL_X0 + 48, 48, COL_BG);
    drawColorTrackRange(SL_X0, SL_X1);
  } else {
    int a = lastCtKnobX - 23, b = lastCtKnobX + 23;     // tylko okolice starej galki
    gfx->fillRect(a, cy - 24, b - a + 1, 48, COL_BG);
    drawColorTrackRange(a, b);
  }
  gfx->fillCircle(x, cy, 19, COL_TEXT);
  gfx->fillCircle(x, cy, 10, ctColor(lampColor));
  lastCtKnobX = x;

  const char *w = (lampColor <= 15) ? "CIEPLA" : (lampColor >= 85 ? "ZIMNA" : "NEUTRALNA");
  drawSliderLabel(312, "BARWA", w);
}

void drawChip(int i, bool pressed) {
  uint16_t bg = pressed ? COL_CARD_P : COL_CARD;
  gfx->fillRoundRect(CHIP_X[i], CHIP_Y, CHIP_W, CHIP_H, CHIP_H / 2, bg);
  gfx->drawRoundRect(CHIP_X[i], CHIP_Y, CHIP_W, CHIP_H, CHIP_H / 2, COL_TRACK2);
  drawCentered(PRESET_NAME[i], CHIP_X[i] + CHIP_W / 2, CHIP_Y + (CHIP_H - 14) / 2, 2, COL_TEXT, bg);
}

// ---------- wywolywane z glownego pliku ----------
void lightDrawAll() {
  lastBrKnobX = lastCtKnobX = -1;
  lightDrawTopBar();
  drawPower(false);
  drawPowerLabel();
  drawBrightness();
  drawColorSlider();
  for (int i = 0; i < 3; i++) drawChip(i, false);
  lampDirty = 0;
}

void lightRefresh() {
  if (dirtyStatus) { lightDrawTopBar(); dirtyStatus = false; }
  if (lampDirty & LD_POWER) {
    drawPower(pressedItem == 1);
    drawPowerLabel();
    dirtyStatus = true;               // naglowek pokazuje stan wlaczenia
  }
  if (lampDirty & (LD_POWER | LD_BRIGHT)) drawBrightness();
  if (lampDirty & LD_COLOR) drawColorSlider();
  lampDirty = 0;
}

void lightReset() {
  lightDown = false;
  activeSlider = 0;
  pressedItem = 0;
}

// 0 = nic; 1 jasnosc, 2 barwa, 3 power, 4..6 presety
int lightHitAny(int x, int y) {
  if (x >= SL_X0 - 30 && x <= SL_X1 + 30) {
    if (abs(y - SL_BR_Y) <= SL_HIT) return 1;
    if (abs(y - SL_CT_Y) <= SL_HIT) return 2;
  }
  int dx = x - PW_X, dy = y - PW_Y;
  if (dx * dx + dy * dy <= (PW_R + 10) * (PW_R + 10)) return 3;
  for (int i = 0; i < 3; i++) {
    if (x >= CHIP_X[i] - 6 && x <= CHIP_X[i] + CHIP_W + 6 && y >= CHIP_Y - 6 && y <= CHIP_Y + CHIP_H + 6)
      return 4 + i;
  }
  return 0;
}

void lightSendPending(bool force) {
  uint32_t now = millis();
  if (!force && now - lastSendMs < 120) return;     // ogranicz ruch na MQTT podczas przeciagania
  if (lampBright != lastSentBr) { lampSendBrightness(lampBright); lastSentBr = lampBright; lastSendMs = now; }
  if (lampColor != lastSentCt)  { lampSendColor(lampColor);       lastSentCt = lampColor;  lastSendMs = now; }
}

void lightTouch(int tx, int ty) {
  uint32_t now = millis();

  if (!lightDown) {                                  // pierwszy kontakt
    lightDown = true;
    lastSentBr = lastSentCt = -1;                    // pierwsza zmiana zawsze zostanie wyslana
    pressedItem = 0;
    int hit = lightHitAny(tx, ty);
    lampUserMs = now;

    if (hit == 1 || hit == 2) {
      activeSlider = hit;
    } else if (hit == 3) {
      pressedItem = 1;
      lampOn = !lampOn;
      lampSendPower(lampOn);
      lampDirty |= LD_POWER | LD_BRIGHT;
    } else if (hit >= 4) {
      int i = hit - 4;
      pressedItem = 2 + i;
      lampBright = PRESET_BR[i];
      lampColor = PRESET_CT[i];
      lampOn = true;
      lampSendBrightness(lampBright);
      lampSendColor(lampColor);
      lampSendPower(true);
      lastSentBr = lampBright;
      lastSentCt = lampColor;
      lampDirty = LD_POWER | LD_BRIGHT | LD_COLOR;
      drawChip(i, true);
    }
  }

  if (activeSlider) {
    lampUserMs = now;
    int v = constrain((tx - SL_X0) * 100 / (SL_X1 - SL_X0), 0, 100);
    if (activeSlider == 1) {
      if (v != lampBright) {
        lampBright = v;
        lampDirty |= LD_BRIGHT;
        if (!lampOn && v > 0) {                      // ruszenie suwakiem wlacza swiatlo
          lampOn = true;
          lampSendPower(true);
          lampDirty |= LD_POWER;
        }
      }
    } else {
      if (v != lampColor) {
        lampColor = v;
        lampDirty |= LD_COLOR;
      }
    }
    lightSendPending(false);
  }
}

void lightRelease() {
  lampUserMs = millis();
  if (activeSlider) lightSendPending(true);          // wyslij koncowa wartosc
  if (pressedItem == 1) {
    pressedItem = 0;
    lampDirty |= LD_POWER;
  } else if (pressedItem >= 2) {
    int i = pressedItem - 2;
    pressedItem = 0;
    drawChip(i, false);
  }
  activeSlider = 0;
  lightDown = false;
}
