// Zakladka: przygaszanie podswietlenia po bezczynnosci.
// Podswietlenie to PWM w CH32V003 (0..255) - WS_CH32_IO::setPwm().
// Pierwszy dotyk przy przygaszonym ekranie tylko go budzi (nie wykonuje akcji).

#define BL_NORMAL        155              // jasnosc normalna (0..255)
#define BL_DIM           120               // jasnosc po bezczynnosci
#define BL_DIM_AFTER_MS  (60UL * 1000UL)  // po ilu ms bez dotyku przygasic
#define BL_OFF_AFTER_MS  0UL              // 0 = nigdy nie gas calkiem; np. (10UL * 60UL * 1000UL)
#define BL_FADE_STEP     12               // krok sciemniania co 20 ms

static int blCur = -1;                    // aktualny poziom PWM (-1 = nieznany)
static int blTarget = BL_NORMAL;
static uint32_t blLastActivity = 0, blLastStep = 0;

static void blWrite(int v) {
  v = constrain(v, 0, 255);
  if (v == blCur) return;
  if (WS_CH32_IO::setPwm(Wire, (uint8_t)v)) blCur = v;
}

void backlightInit() {
  blLastActivity = millis();
  blTarget = BL_NORMAL;
  blWrite(BL_NORMAL);
}

// Wolaj przy kazdym dotyku. Zwraca true, jesli ekran byl przygaszony/wygaszony
// (taki dotyk ma tylko obudzic ekran).
bool backlightActivity() {
  bool wasDim = (blTarget != BL_NORMAL);
  blLastActivity = millis();
  if (wasDim) {
    blTarget = BL_NORMAL;
    blWrite(BL_NORMAL);
  }
  return wasDim;
}

void backlightLoop(uint32_t now) {
  uint32_t idle = now - blLastActivity;
  if (BL_OFF_AFTER_MS && idle >= BL_OFF_AFTER_MS) blTarget = 0;
  else if (idle >= BL_DIM_AFTER_MS)               blTarget = BL_DIM;
  else                                            blTarget = BL_NORMAL;

  if (blCur != blTarget && now - blLastStep >= 20) {
    blLastStep = now;
    int next = blTarget;                            // w gore: od razu
    if (blCur > blTarget) next = max(blTarget, blCur - BL_FADE_STEP);   // w dol: plynnie
    blWrite(next);
  }
}
