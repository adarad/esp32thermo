// ESP32-S3-Touch-LCD-4 (V4.0) - panel termostatu pokojowego
// Baza: dzialajacy przyklad Waveshare (Arduino_GFX + GT911 + WS_CH32_IO)
// Komunikacja (MQTT / Wi-Fi / RS485 ...) do dobudowania - patrz sekcja API.

#include <Wire.h>
#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "TouchDrvGT911.hpp"
#include <SPI.h>
#include "WS_CH32_IO.h"

#include "HWCDC.h"
HWCDC USBSerial;

// ===================== Sprzet (jak w dzialajacym przykladzie) =====================
TouchDrvGT911 GT911;
int16_t tx_raw[5], ty_raw[5];
uint8_t gt911_i2c_addr = 0;
bool gt911_available = false;

Arduino_DataBus *bus = new Arduino_SWSPI(
    GFX_NOT_DEFINED /* DC */, 42 /* CS */,
    2 /* SCK */, 1 /* MOSI */, GFX_NOT_DEFINED /* MISO */);

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    40 /* DE */, 39 /* VSYNC */, 38 /* HSYNC */, 41 /* PCLK */,
    46 /* R0 */, 3 /* R1 */, 8 /* R2 */, 18 /* R3 */, 17 /* R4 */,
    14 /* G0 */, 13 /* G1 */, 12 /* G2 */, 11 /* G3 */, 10 /* G4 */, 9 /* G5 */,
    5 /* B0 */, 45 /* B1 */, 48 /* B2 */, 47 /* B3 */, 21 /* B4 */,
    1 /* hsync_polarity */, 10 /* hsync_front_porch */, 8 /* hsync_pulse_width */, 50 /* hsync_back_porch */,
    1 /* vsync_polarity */, 10 /* vsync_front_porch */, 8 /* vsync_pulse_width */, 20 /* vsync_back_porch */);
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    480 /* width */, 480 /* height */, rgbpanel, 2 /* rotation */, true /* auto_flush */,
    bus, GFX_NOT_DEFINED /* RST */, st7701_type1_init_operations, sizeof(st7701_type1_init_operations));

// ===================== Ustawienia aplikacji =====================
#define SIMULATE_TEMPERATURE false  // true = symulacja pomiaru (do testow bez sieci); false = temperatura z czujnika przez MQTT
static const char *ROOM_NAME = "SALON";

static const float TEMP_MIN = 10.0f;
static const float TEMP_MAX = 30.0f;
static const float TEMP_STEP = 0.5f;
static const float ECO_OFFSET = 2.0f;   // tryb ECO: cel = nastawa - 2.0
static const float HYST_ON = 0.3f;      // wlacz grzanie gdy temp < cel - 0.3

// ===================== Kolory =====================
#define TC(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
static const uint16_t COL_BG     = TC(10, 12, 18);
static const uint16_t COL_CARD   = TC(30, 34, 46);
static const uint16_t COL_CARD_P = TC(55, 62, 84);
static const uint16_t COL_TRACK  = TC(42, 47, 62);
static const uint16_t COL_TRACK2 = TC(80, 86, 104);
static const uint16_t COL_TEXT   = TC(240, 243, 250);
static const uint16_t COL_MUTED  = TC(135, 144, 165);
static const uint16_t COL_HEAT   = TC(255, 128, 50);
static const uint16_t COL_COOL   = TC(70, 170, 255);
static const uint16_t COL_OK     = TC(80, 215, 140);
static const uint16_t COL_ERR    = TC(235, 80, 80);
static const uint16_t COL_LAMP   = TC(255, 190, 90);

// ===================== Uklad ekranu 480x480 =====================
static const int W = 480;
static const int CX = 240, CY = 240;
static const int RING_R = 160;
static const float ARC_START = 135.0f;   // stopnie (0 = prawo, 90 = dol), luk 270 stopni
static const float ARC_SWEEP = 270.0f;
static const int MARK_R = RING_R - 16;

static const int BTN_Y = 415, BTN_R = 42;
static const int BTN_MINUS_X = 85, BTN_PLUS_X = 395;
static const int PILL_X = 155, PILL_Y = 383, PILL_W = 170, PILL_H = 64;

// ===================== Stan =====================
enum Mode { MODE_AUTO = 0, MODE_ECO = 1, MODE_OFF = 2 };
enum Btn { BTN_NONE = 0, BTN_MINUS = 1, BTN_PLUS = 2, BTN_MODE = 3 };

float setpoint = 22.0f;
float curTemp = 21.5f;
float humidity = 45.0f;
Mode mode = MODE_AUTO;
bool heating = false;
bool sensorStale = false;   // brak swiezego odczytu z czujnika -> grzanie zablokowane (ustawia ha_mqtt.ino)
bool netOnline = false;     // Wi-Fi + MQTT polaczone (ustawia ha_mqtt.ino)

bool dirtyRing = true, dirtyStatus = true, dirtyMode = true;

bool knobDrawn = false, markDrawn = false;
float lastKnobDeg = 0, lastMarkDeg = 0;

Btn heldBtn = BTN_NONE;
uint32_t pressStart = 0, lastRepeat = 0, lastTouchMs = 0;
uint32_t lastSim = 0;

// ===================== Strony i lampa (light_page.ino, pages.ino) =====================
#define LD_POWER  1
#define LD_BRIGHT 2
#define LD_COLOR  4
uint8_t uiPage = 0;         // 0 = termostat, 1 = swiatlo
bool lampOn = false;
int lampBright = 50;        // 0..100 %
int lampColor = 40;         // 0 = cieple LED, 100 = zimne LED
uint8_t lampDirty = 0;
uint32_t lampUserMs = 0;    // ostatni dotyk na stronie swiatla (wtedy ignorujemy echo stanu z MQTT)

// ===================== Logika =====================
float effectiveTarget() {
  return (mode == MODE_ECO) ? (setpoint - ECO_OFFSET) : setpoint;
}

uint16_t accentColor() {
  if (mode == MODE_OFF) return COL_TRACK2;
  return heating ? COL_HEAT : COL_COOL;
}

void updateHeating() {
  bool prev = heating;
  if (mode == MODE_OFF || sensorStale) {
    heating = false;
  } else {
    float t = effectiveTarget();
    if (!heating && curTemp < t - HYST_ON) heating = true;
    else if (heating && curTemp >= t) heating = false;
  }
  if (heating != prev) {
    dirtyRing = true;
    dirtyStatus = true;
  }
}

// ===================== API do przyszlej komunikacji =====================
// Wywoluj z kodu MQTT / Wi-Fi / RS485:
void setCurrentTemperature(float t) { curTemp = t; dirtyRing = true; updateHeating(); }
void setHumidity(float h) { humidity = h; dirtyRing = true; }
void setSetpointRemote(float sp) {
  setpoint = constrain(sp, TEMP_MIN, TEMP_MAX);
  dirtyRing = true;
  updateHeating();
}
void setModeRemote(Mode m) { mode = m; dirtyMode = dirtyStatus = dirtyRing = true; updateHeating(); }

// Wywolywane gdy uzytkownik zmieni nastawe/tryb na ekranie:
void onUserChanged() {
  USBSerial.printf("USER: setpoint=%.1f mode=%d heating=%d\n", setpoint, (int)mode, heating ? 1 : 0);
  // TODO: tutaj publikacja do sieci (MQTT itp.)
}

// ===================== Rysowanie =====================
void drawCentered(const char *s, int cx, int y, int size, uint16_t fg, uint16_t bg) {
  int w = strlen(s) * 6 * size;
  gfx->setTextSize(size);
  gfx->setTextColor(fg, bg);
  gfx->setCursor(cx - w / 2, y);
  gfx->print(s);
}

void drawDegree(int x, int y, int r, uint16_t col) {
  gfx->drawCircle(x, y, r, col);
  gfx->drawCircle(x, y, r - 1, col);
}

float valToDeg(float v) {
  v = constrain(v, TEMP_MIN, TEMP_MAX);
  return ARC_START + ARC_SWEEP * (v - TEMP_MIN) / (TEMP_MAX - TEMP_MIN);
}

void ringPoint(float deg, int r, int &px, int &py) {
  float a = deg * DEG_TO_RAD;
  px = CX + (int)lroundf(cosf(a) * r);
  py = CY + (int)lroundf(sinf(a) * r);
}

// Pasek gorny
void drawTopBar() {
  gfx->fillRect(0, 0, W, 58, COL_BG);
  gfx->setTextSize(3);
  gfx->setTextColor(COL_TEXT, COL_BG);
  gfx->setCursor(24, 17);
  gfx->print(ROOM_NAME);

  // wskaznik polaczenia z Home Assistant (MQTT)
  gfx->setTextSize(2);
  gfx->setTextColor(netOnline ? COL_OK : COL_TRACK2, COL_BG);
  gfx->setCursor(24 + (int)strlen(ROOM_NAME) * 18 + 14, 22);
  gfx->print("HA");

  const char *txt;
  uint16_t col;
  if (mode == MODE_OFF)      { txt = "WYLACZONY"; col = COL_MUTED; }
  else if (sensorStale)      { txt = "BRAK CZUJNIKA"; col = COL_ERR; }
  else if (heating)          { txt = "GRZANIE";   col = COL_HEAT; }
  else                       { txt = "CZUWANIE";  col = COL_OK; }

  int w = strlen(txt) * 6 * 2;
  int x = W - 24 - w;
  gfx->fillCircle(x - 16, 29, 6, col);
  gfx->setTextSize(2);
  gfx->setTextColor(col, COL_BG);
  gfx->setCursor(x, 22);
  gfx->print(txt);

  gfx->drawFastHLine(24, 57, W - 48, COL_TRACK);
}

// Srodek: etykiety, aktualna temp., nastawa, wilgotnosc.
// Rysuje tylko to, co sie zmienilo (full = wszystko, po wyczyszczeniu ekranu).
void drawCenter(bool full) {
  static char lastCur[12], lastSet[12], lastHum[24];
  static uint16_t lastAcc = 0;
  char buf[24];
  uint16_t acc = accentColor();

  if (full) {
    drawCentered("TEMPERATURA", CX, 128, 2, COL_MUTED, COL_BG);
    drawCentered("USTAWIONA", CX, 234, 2, COL_MUTED, COL_BG);
    lastCur[0] = lastSet[0] = lastHum[0] = 0;
  }

  snprintf(buf, sizeof(buf), "%4.1f", curTemp);
  if (strcmp(buf, lastCur) != 0) {
    gfx->setTextSize(8);
    gfx->setTextColor(COL_TEXT, COL_BG);
    gfx->setCursor(126, 160);
    gfx->print(buf);
    strcpy(lastCur, buf);
  }
  // jednostka: zawsze (znacznik na pierscieniu moze ja zahaczyc)
  drawDegree(330, 170, 6, COL_MUTED);
  gfx->setTextSize(4);
  gfx->setTextColor(COL_MUTED, COL_BG);
  gfx->setCursor(342, 164);
  gfx->print("C");

  snprintf(buf, sizeof(buf), "%4.1f", setpoint);
  if (full || strcmp(buf, lastSet) != 0 || acc != lastAcc) {
    gfx->setTextSize(4);
    gfx->setTextColor(acc, COL_BG);
    gfx->setCursor(168, 256);
    gfx->print(buf);
    drawDegree(272, 262, 4, acc);
    gfx->setTextSize(3);
    gfx->setTextColor(acc, COL_BG);
    gfx->setCursor(282, 260);
    gfx->print("C");
    strcpy(lastSet, buf);
    lastAcc = acc;
  }

  snprintf(buf, sizeof(buf), "WILGOTNOSC %2.0f%%", humidity);
  if (strcmp(buf, lastHum) != 0) {
    drawCentered(buf, CX, 304, 2, COL_MUTED, COL_BG);
    strcpy(lastHum, buf);
  }
}

// Stan lampy pod wilgotnoscia - dotkniecie srodka ekranu przelacza swiatlo (pages.ino)
void drawLampIndicator() {
  char buf[20];
  uint16_t col = lampOn ? COL_LAMP : COL_MUTED;
  if (lampOn) snprintf(buf, sizeof(buf), "SWIATLO %3d%%", lampBright);
  else        snprintf(buf, sizeof(buf), "SWIATLO  WYL");
  gfx->fillCircle(154, 344, 7, COL_BG);
  if (lampOn) gfx->fillCircle(154, 344, 6, col);
  else        gfx->drawCircle(154, 344, 6, COL_TRACK2);
  drawCentered(buf, CX, 336, 2, col, COL_BG);
}

// Pierscien nastawy: segmenty co 1,5 stopnia. Pamietamy kolor kazdego segmentu
// i przerysowujemy tylko te, ktore sie zmienily (plus okolice gałki / znacznika).
static const int NSEG = 181;
static const float SEG_STEP = 1.5f;
uint16_t segCol[NSEG];

void ringReset() {
  for (int i = 0; i < NSEG; i++) segCol[i] = 0xFFFF;   // 0xFFFF = "nieznany"
  knobDrawn = markDrawn = false;
}

void ringInvalidate(float deg, float span) {
  for (int i = 0; i < NSEG; i++) {
    float d = ARC_START + i * SEG_STEP;
    if (fabsf(d - deg) <= span) segCol[i] = 0xFFFF;
  }
}

void drawRing() {
  int px, py;
  if (knobDrawn) {
    ringPoint(lastKnobDeg, RING_R, px, py);
    gfx->fillCircle(px, py, 17, COL_BG);
    ringInvalidate(lastKnobDeg, 9.5f);        // gałka zamazala sasiednie segmenty
  }
  if (markDrawn) {
    ringPoint(lastMarkDeg, MARK_R, px, py);
    gfx->fillCircle(px, py, 8, COL_BG);
  }

  float setDeg = valToDeg(setpoint);
  float curDeg = valToDeg(curTemp);
  uint16_t acc = accentColor();

  for (int i = 0; i < NSEG; i++) {
    float d = ARC_START + i * SEG_STEP;
    uint16_t col = (d <= setDeg) ? acc : COL_TRACK;
    if (segCol[i] != col) {
      ringPoint(d, RING_R, px, py);
      gfx->fillCircle(px, py, 6, col);
      segCol[i] = col;
    }
  }

  ringPoint(setDeg, RING_R, px, py);
  gfx->fillCircle(px, py, 13, COL_TEXT);
  gfx->fillCircle(px, py, 7, acc);
  lastKnobDeg = setDeg;
  knobDrawn = true;

  ringPoint(curDeg, MARK_R, px, py);
  gfx->fillCircle(px, py, 5, COL_TEXT);
  lastMarkDeg = curDeg;
  markDrawn = true;

  drawCenter(false);   // tylko zmienione elementy; jednostka przy temp. zawsze
}

void drawRoundBtn(int cx, int cy, bool plus, bool pressed) {
  gfx->fillCircle(cx, cy, BTN_R, pressed ? COL_CARD_P : COL_CARD);
  gfx->drawCircle(cx, cy, BTN_R, COL_TRACK2);
  uint16_t c = pressed ? accentColor() : COL_TEXT;
  gfx->fillRect(cx - 15, cy - 3, 30, 6, c);
  if (plus) gfx->fillRect(cx - 3, cy - 15, 6, 30, c);
}

void drawModePill(bool pressed) {
  gfx->fillRoundRect(PILL_X, PILL_Y, PILL_W, PILL_H, PILL_H / 2, pressed ? COL_CARD_P : COL_CARD);
  gfx->drawRoundRect(PILL_X, PILL_Y, PILL_W, PILL_H, PILL_H / 2, COL_TRACK2);
  const char *lbl;
  uint16_t col;
  switch (mode) {
    case MODE_AUTO: lbl = "AUTO"; col = COL_OK; break;
    case MODE_ECO:  lbl = "ECO";  col = COL_COOL; break;
    default:        lbl = "WYL";  col = COL_MUTED; break;
  }
  drawCentered(lbl, PILL_X + PILL_W / 2, PILL_Y + (PILL_H - 21) / 2, 3, col,
               pressed ? COL_CARD_P : COL_CARD);
}

void drawButton(Btn b, bool pressed) {
  switch (b) {
    case BTN_MINUS: drawRoundBtn(BTN_MINUS_X, BTN_Y, false, pressed); break;
    case BTN_PLUS:  drawRoundBtn(BTN_PLUS_X, BTN_Y, true, pressed); break;
    case BTN_MODE:  drawModePill(pressed); break;
    default: break;
  }
}

void drawAll() {
  gfx->fillScreen(COL_BG);
  ringReset();
  drawCenter(true);
  drawTopBar();
  drawRing();
  drawButton(BTN_MINUS, false);
  drawButton(BTN_PLUS, false);
  drawButton(BTN_MODE, false);
  drawPageDots();
  drawLampIndicator();
  dirtyRing = dirtyStatus = dirtyMode = false;
}

void refreshUI() {
  if (uiPage != 0) { lightRefresh(); return; }
  if (lampDirty) { drawLampIndicator(); lampDirty = 0; }
  if (dirtyStatus) { drawTopBar(); dirtyStatus = false; }
  if (dirtyRing)   { drawRing();   dirtyRing = false; }
  if (dirtyMode) {
    drawButton(BTN_MODE, heldBtn == BTN_MODE);
    drawButton(BTN_MINUS, heldBtn == BTN_MINUS);  // kolor akcentu przy wcisnieciu
    drawButton(BTN_PLUS, heldBtn == BTN_PLUS);
    dirtyMode = false;
  }
}

// ===================== Dotyk =====================
Btn hitTest(int x, int y) {
  int dx = x - BTN_MINUS_X, dy = y - BTN_Y;
  if (dx * dx + dy * dy <= 52 * 52) return BTN_MINUS;
  dx = x - BTN_PLUS_X;
  if (dx * dx + dy * dy <= 52 * 52) return BTN_PLUS;
  if (x >= PILL_X - 8 && x <= PILL_X + PILL_W + 8 && y >= PILL_Y - 8 && y <= PILL_Y + PILL_H + 8)
    return BTN_MODE;
  return BTN_NONE;
}

void applyAction(Btn b) {
  if (b == BTN_MINUS) {
    setpoint = max(TEMP_MIN, setpoint - TEMP_STEP);
  } else if (b == BTN_PLUS) {
    setpoint = min(TEMP_MAX, setpoint + TEMP_STEP);
  } else if (b == BTN_MODE) {
    mode = (Mode)(((int)mode + 1) % 3);
    dirtyMode = true;
    dirtyStatus = true;
  }
  dirtyRing = true;
  updateHeating();
  onUserChanged();
}

void handleTouch(int tx, int ty) {
  uint32_t now = millis();
  lastTouchMs = now;
  Btn b = hitTest(tx, ty);

  if (b != heldBtn) {
    if (heldBtn != BTN_NONE) drawButton(heldBtn, false);
    heldBtn = b;
    if (b != BTN_NONE) {
      pressStart = lastRepeat = now;
      drawButton(b, true);
      applyAction(b);
    }
  } else if ((b == BTN_MINUS || b == BTN_PLUS) && now - pressStart > 450 && now - lastRepeat > 130) {
    lastRepeat = now;
    applyAction(b);
  }
}

void handleRelease() {
  if (heldBtn != BTN_NONE && millis() - lastTouchMs > 40) {
    drawButton(heldBtn, false);
    heldBtn = BTN_NONE;
  }
}

// ===================== GT911 (jak w dzialajacym przykladzie) =====================
void i2c_scan() {
  USBSerial.println("Scanning I2C bus...");
  for (byte address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      USBSerial.print("I2C device found at 0x");
      USBSerial.println(address, HEX);
      if (address == GT911_SLAVE_ADDRESS_L || address == GT911_SLAVE_ADDRESS_H) {
        gt911_i2c_addr = address;
      }
    }
  }
}

bool init_gt911_with_probe(int sda_pin, int scl_pin) {
  Wire.begin(sda_pin, scl_pin);
  delay(100);
  i2c_scan();
  if (gt911_i2c_addr == 0) {
    USBSerial.println("GT911 not found in I2C scan");
    return false;
  }
  GT911.setPins(-1, -1);
  if (GT911.begin(Wire, gt911_i2c_addr, sda_pin, scl_pin)) {
    USBSerial.print("GT911 OK at 0x");
    USBSerial.println(gt911_i2c_addr, HEX);
    return true;
  }
  USBSerial.println("GT911 init failed");
  return false;
}

// ===================== setup / loop =====================
void setup() {
  USBSerial.begin(115200);
  USBSerial.setTxTimeoutMs(0);   // log nie blokuje petli, gdy monitor portu jest zamkniety

  if (!WS_CH32_IO::begin(Wire, WS_CH32_IO::DEFAULT_I2C_SDA, WS_CH32_IO::DEFAULT_I2C_SCL,
                         WS_CH32_IO::DEFAULT_I2C_FREQ, &USBSerial)) {
    USBSerial.println("CH32V003 IO expander init failed");
  }

  gfx->begin();
  gfx->fillScreen(COL_BG);

  gt911_available = init_gt911_with_probe(15, 7);
  if (gt911_available) {
    GT911.setMaxTouchPoint(1);
  }

  persistLoad();    // wczytaj zapamietana nastawe i tryb (ha_mqtt.ino)
  updateHeating();
  drawAll();
  lastSim = millis();
  backlightInit();  // przygaszanie (backlight.ino)
  networkSetup();   // Wi-Fi + MQTT (ha_mqtt.ino)
}

void loop() {
  uint32_t t0 = millis();
  uint32_t now = t0;

#if SIMULATE_TEMPERATURE
  if (now - lastSim >= 3000) {
    lastSim = now;
    curTemp += heating ? 0.1f : -0.05f;
    curTemp = constrain(curTemp, 5.0f, 35.0f);
    dirtyRing = true;
    updateHeating();
  }
#endif

  if (gt911_available) {
    uint8_t touched = GT911.getPoint(tx_raw, ty_raw, GT911.getSupportTouchPoint());
    if (touched > 0) {
      // rotation 2 (jak w oryginalnym przykladzie)
      int tx = gfx->width() - tx_raw[0];
      int ty = gfx->height() - ty_raw[0];
      uiTouch(tx, ty, true);
    } else {
      uiTouch(0, 0, false);
    }
  }
  uint32_t t1 = millis();

  refreshUI();      // najpierw ekran: odpowiedz na dotyk nie czeka na siec
  uint32_t t2 = millis();

  networkLoop();    // MQTT, publikacja stanu, zapis nastaw (ha_mqtt.ino)
  uint32_t t3 = millis();

  backlightLoop(t3);  // przygaszanie (backlight.ino)

  // diagnostyka: wypisz, ktora czesc petli trwala dlugo
  static uint32_t lastLagLog = 0;
  if ((t1 - t0 > 120 || t2 - t1 > 120 || t3 - t2 > 120) && t3 - lastLagLog > 2000) {
    lastLagLog = t3;
    USBSerial.printf("LAG: dotyk=%lums ekran=%lums siec=%lums\n",
                     (unsigned long)(t1 - t0), (unsigned long)(t2 - t1), (unsigned long)(t3 - t2));
  }
  delay(10);
}
