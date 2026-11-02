// Zakladka: przelaczanie stron (0 = termostat, 1 = swiatlo) i obsluga dotyku
//  - przesuniecie palcem w lewo/prawo (termostat: w srodkowej czesci ekranu, swiatlo: z gornego paska)
//  - dotkniecie kropek na dole ekranu (lewa polowa = termostat, prawa = swiatlo)
//  - dotkniecie srodka ekranu termostatu = wlacz/wylacz swiatlo
//  - pierwszy dotyk przy przygaszonym ekranie tylko go budzi
// Dotyk z glownego pliku trafia tutaj (uiTouch) i jest kierowany do aktywnej strony.

void drawPageDots() {
  gfx->fillRect(180, 462, 120, 18, COL_BG);
  for (int i = 0; i < 2; i++) {
    int cx = 222 + i * 36;
    if (i == uiPage) gfx->fillCircle(cx, 471, 5, COL_TEXT);
    else gfx->fillCircle(cx, 471, 4, COL_TRACK2);
  }
}

void setPage(int p) {
  if (p == uiPage) return;
  uiPage = (uint8_t)p;
  if (uiPage == 0) {
    heldBtn = BTN_NONE;
    drawAll();                 // termostat (rysuje tez kropki i wskaznik lampy)
  } else {
    lightReset();
    gfx->fillScreen(COL_BG);
    lightDrawAll();
    drawPageDots();
  }
}

// Srodek termostatu: kolo w pierscieniu + pasek ze wskaznikiem lampy pod wilgotnoscia
static bool centerZone(int x, int y) {
  int dx = x - CX, dy = y - CY;
  if (dx * dx + dy * dy <= 130 * 130) return true;
  return (y >= 326 && y <= 368 && abs(dx) <= 100);
}

void lampToggle() {
  lampOn = !lampOn;
  lampUserMs = millis();
  lampSendPower(lampOn);
  if (lampOn && lampBright == 0) {          // wlaczenie przy zerowej jasnosci - ustaw rozsadna
    lampBright = 50;
    lampSendBrightness(lampBright);
  }
  lampDirty |= LD_POWER | LD_BRIGHT;
  USBSerial.printf("Swiatlo (srodek termostatu): %s\n", lampOn ? "ON" : "OFF");
}

void uiTouch(int x, int y, bool touched) {
  static bool wasDown = false, swipeOk = false, swiping = false, navHit = false;
  static bool swallow = false, centerCand = false;
  static int sx = 0, sy = 0, lx = 0, ly = 0;
  static uint32_t lastDownMs = 0, downMs = 0;
  uint32_t now = millis();

  if (touched) {
    bool wasDim = backlightActivity();
    lastDownMs = now;
    lx = x;
    ly = y;
    if (!wasDown) {                                   // poczatek dotyku
      wasDown = true;
      swiping = navHit = centerCand = false;
      sx = x;
      sy = y;
      downMs = now;
      swallow = wasDim;                               // dotyk budzacy ekran nie robi nic wiecej
      if (swallow) return;
      if (y >= 462 && x >= 160 && x <= 320) {         // kropki stron
        navHit = true;
        setPage(x < 240 ? 0 : 1);
        return;
      }
      swipeOk = (uiPage == 0) ? (y > 70 && y < 360) : (y < 62);
      centerCand = (uiPage == 0) && centerZone(x, y);
    }
    if (swallow || navHit) return;
    if (swipeOk && abs(x - sx) > 40) swiping = true;  // od teraz to przesuniecie, nie klikniecie
    if (swiping) return;
    if (uiPage == 0) {
      if (!centerCand) handleTouch(x, y);             // srodek obslugujemy dopiero po puszczeniu
    } else {
      lightTouch(x, y);
    }
  } else {
    if (uiPage == 0) handleRelease();
    if (wasDown && now - lastDownMs > 50) {           // palec naprawde puszczony
      wasDown = false;
      if (!navHit && !swallow) {
        if (uiPage == 1) lightRelease();
        int dx = lx - sx, dy = ly - sy;
        if (swipeOk && abs(dx) > 90 && abs(dy) < abs(dx) / 2) {
          if (dx < 0 && uiPage == 0) setPage(1);
          else if (dx > 0 && uiPage == 1) setPage(0);
        } else if (centerCand && !swiping && abs(dx) < 25 && abs(dy) < 25 && lastDownMs - downMs < 700) {
          lampToggle();                                // krotkie dotkniecie srodka
        }
      }
      navHit = swiping = swipeOk = swallow = centerCand = false;
    }
  }
}
