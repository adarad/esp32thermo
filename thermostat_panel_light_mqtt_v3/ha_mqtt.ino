// Zakladka: Wi-Fi + MQTT + Home Assistant (MQTT Discovery, encja "climate")
// Wymaga biblioteki: PubSubClient (Nick O'Leary) >= 2.8
// Korzysta z funkcji i zmiennych z glownego pliku (setpoint, mode, heating, curTemp, ...).

#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include "config.h"
#include "lamp_config.h"

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);
static Preferences prefs;

static char tSetTemp[96], tSetMode[96], tSetPreset[96];

// stan ostatnio opublikowany (do wykrywania zmian)
static float pubTemp = -999, pubSet = -999, pubHum = -999;
static int pubMode = -1, pubHeat = -1;
static bool forcePublish = true;

// czujnik
static bool gotSensor = false;
static bool humFromSensor = false;
static uint32_t lastSensorMs = 0;

// zapis nastaw do pamieci nieulotnej
static float savedSp = 22.0f, seenSp = 22.0f;
static int savedMode = 0, seenMode = 0;
static uint32_t persistChangeMs = 0;

// ---------------------------------------------------------------
// Pomocnicze
// ---------------------------------------------------------------
void pubTopic(const char *suffix, const char *payload) {
  char t[96];
  snprintf(t, sizeof(t), "%s/%s", BASE_TOPIC, suffix);
  mqtt.publish(t, payload, true);
}

// Wartosc z payloadu: sama liczba albo JSON z podanym kluczem
bool parseValue(const char *payload, const char *key, float *out) {
  if (key && key[0]) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(payload, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ') p++;
    if (*p == '"') p++;
    *out = (float)atof(p);
    return true;
  }
  // sama liczba - tolerujemy spacje i cudzyslowy, np. "24.1875"
  const char *p = payload;
  while (*p == ' ' || *p == '"' || *p == '\'') p++;
  char *end;
  *out = strtof(p, &end);
  return end != p;
}

// ---------------------------------------------------------------
// Home Assistant MQTT Discovery
// ---------------------------------------------------------------
void publishDiscovery() {
  static char buf[1100];
  snprintf(buf, sizeof(buf), R"J({"name":null,"uniq_id":"%s","~":"%s",
"availability_topic":"~/status",
"current_temperature_topic":"~/current_temp","current_humidity_topic":"~/humidity",
"temperature_command_topic":"~/set_temp","temperature_state_topic":"~/target_temp",
"mode_command_topic":"~/set_mode","mode_state_topic":"~/mode","modes":["off","heat"],
"preset_mode_command_topic":"~/set_preset","preset_mode_state_topic":"~/preset","preset_modes":["eco"],
"action_topic":"~/action",
"min_temp":%.0f,"max_temp":%.0f,"temp_step":0.5,"precision":0.1,"temperature_unit":"C",
"device":{"identifiers":["%s"],"name":"%s","manufacturer":"Waveshare","model":"ESP32-S3-Touch-LCD-4"}})J",
           DEVICE_ID, BASE_TOPIC, TEMP_MIN, TEMP_MAX, DEVICE_ID, FRIENDLY_NAME);

  char topic[96];
  snprintf(topic, sizeof(topic), "homeassistant/climate/%s/config", DEVICE_ID);
  bool ok = mqtt.publish(topic, buf, true);
  USBSerial.printf("MQTT discovery: %s\n", ok ? "OK" : "BLAD (za maly bufor?)");
}

void publishChanges() {
  char b[24];
  if (forcePublish || fabsf(curTemp - pubTemp) >= 0.05f) {
    snprintf(b, sizeof(b), "%.1f", curTemp);
    pubTopic("current_temp", b);
    pubTemp = curTemp;
  }
  if (humFromSensor && (forcePublish || fabsf(humidity - pubHum) >= 0.5f)) {
    snprintf(b, sizeof(b), "%.0f", humidity);
    pubTopic("humidity", b);
    pubHum = humidity;
  }
  if (forcePublish || fabsf(setpoint - pubSet) >= 0.01f) {
    snprintf(b, sizeof(b), "%.1f", setpoint);
    pubTopic("target_temp", b);
    pubSet = setpoint;
  }
  if (forcePublish || (int)mode != pubMode) {
    pubTopic("mode", mode == MODE_OFF ? "off" : "heat");
    pubTopic("preset", mode == MODE_ECO ? "eco" : "none");
    pubMode = (int)mode;
  }
  int h = heating ? 1 : 0;
  if (forcePublish || h != pubHeat || pubMode != (int)mode) {
    pubTopic("action", mode == MODE_OFF ? "off" : (heating ? "heating" : "idle"));
    pubHeat = h;
  }
  forcePublish = false;
}

// ---------------------------------------------------------------
// Lampa (strona ze swiatlem) - proste tematy, opis w lamp_config.h
// ---------------------------------------------------------------
static void lampPub(const char *name, const char *payload) {
  if (!mqtt.connected()) return;
  char t[96];
  snprintf(t, sizeof(t), "%s/%s/set", LAMP_BASE, name);
  mqtt.publish(t, payload, false);
}

void lampSendPower(bool on) { lampPub("power", on ? "ON" : "OFF"); }
void lampSendBrightness(int pct) { char b[8]; snprintf(b, sizeof(b), "%d", pct); lampPub("brightness", b); }
void lampSendColor(int pct) { char b[8]; snprintf(b, sizeof(b), "%d", pct); lampPub("color", b); }

static void lampSubscribe() {
  const char *names[3] = {"power", "brightness", "color"};
  for (int i = 0; i < 3; i++) {
    char t[96];
    snprintf(t, sizeof(t), "%s/%s", LAMP_BASE, names[i]);
    mqtt.subscribe(t);
  }
}

// true = to byla wiadomosc lampy (obsluzona)
static bool lampMessage(const char *topic, const char *msg) {
  char prefix[96];
  snprintf(prefix, sizeof(prefix), "%s/", LAMP_BASE);
  size_t n = strlen(prefix);
  if (strncmp(topic, prefix, n) != 0) return false;
  const char *sfx = topic + n;
  bool isPower = strcmp(sfx, "power") == 0;
  bool isBr = strcmp(sfx, "brightness") == 0;
  bool isCt = strcmp(sfx, "color") == 0;
  if (!isPower && !isBr && !isCt) return false;

  // tuz po dotyku uzytkownika ignorujemy echo (suwak nie "skacze")
  if (lampUserMs && millis() - lampUserMs < 800) return true;

  if (isPower) {
    bool on = strcasecmp(msg, "ON") == 0 || strcmp(msg, "1") == 0 || strcasecmp(msg, "true") == 0;
    if (on != lampOn) { lampOn = on; lampDirty |= LD_POWER | LD_BRIGHT; }
  } else {
    float v;
    if (parseValue(msg, "", &v)) {
      int p = constrain((int)lroundf(v), 0, 100);
      if (isBr && p != lampBright) { lampBright = p; lampDirty |= LD_BRIGHT; }
      if (isCt && p != lampColor) { lampColor = p; lampDirty |= LD_COLOR; }
    }
  }
  return true;
}

// ---------------------------------------------------------------
// Odbior wiadomosci MQTT
// ---------------------------------------------------------------
void mqttCallback(char *topic, byte *payload, unsigned int len) {
  char msg[256];
  if (len >= sizeof(msg)) len = sizeof(msg) - 1;
  memcpy(msg, payload, len);
  msg[len] = 0;

  if (lampMessage(topic, msg)) return;   // strona ze swiatlem

  if (strcmp(topic, tSetTemp) == 0) {
    float v;
    if (parseValue(msg, "", &v)) {
      v = roundf(v / TEMP_STEP) * TEMP_STEP;
      setSetpointRemote(v);
      USBSerial.printf("HA: nastawa = %.1f\n", setpoint);
    }
  } else if (strcmp(topic, tSetMode) == 0) {
    if (strcmp(msg, "off") == 0) setModeRemote(MODE_OFF);
    else if (strcmp(msg, "heat") == 0 && mode == MODE_OFF) setModeRemote(MODE_AUTO);
    USBSerial.printf("HA: tryb = %s\n", msg);
  } else if (strcmp(topic, tSetPreset) == 0) {
    if (mode != MODE_OFF) setModeRemote(strcmp(msg, "eco") == 0 ? MODE_ECO : MODE_AUTO);
    USBSerial.printf("HA: preset = %s\n", msg);
  } else if (strcmp(topic, SENSOR_TEMP_TOPIC) == 0) {
    float v;
    USBSerial.printf("Czujnik: payload='%s'\n", msg);
    if (parseValue(msg, SENSOR_TEMP_JSON_KEY, &v) && v > -40 && v < 80) {
      gotSensor = true;
      lastSensorMs = millis();
      if (sensorStale) { sensorStale = false; dirtyStatus = true; }
      setCurrentTemperature(v);
    }
  } else if (SENSOR_HUM_TOPIC[0] && strcmp(topic, SENSOR_HUM_TOPIC) == 0) {
    float v;
    if (parseValue(msg, SENSOR_HUM_JSON_KEY, &v) && v >= 0 && v <= 100) {
      humFromSensor = true;
      setHumidity(v);
    }
  }
  forcePublish = true;  // potwierdz HA aktualny stan
}

// ---------------------------------------------------------------
// Polaczenie
// ---------------------------------------------------------------
bool mqttConnect() {
  char clientId[48], will[96];
  snprintf(clientId, sizeof(clientId), "%s-%04X", DEVICE_ID, (uint16_t)(ESP.getEfuseMac() >> 32));
  snprintf(will, sizeof(will), "%s/status", BASE_TOPIC);

  bool ok;
  if (MQTT_USER[0]) ok = mqtt.connect(clientId, MQTT_USER, MQTT_PASS, will, 0, true, "offline");
  else              ok = mqtt.connect(clientId, nullptr, nullptr, will, 0, true, "offline");
  if (!ok) {
    USBSerial.printf("MQTT: blad polaczenia, rc=%d\n", mqtt.state());
    return false;
  }
  USBSerial.println("MQTT: polaczono");

  pubTopic("status", "online");
  publishDiscovery();

  mqtt.subscribe(tSetTemp);
  mqtt.subscribe(tSetMode);
  mqtt.subscribe(tSetPreset);
  mqtt.subscribe(SENSOR_TEMP_TOPIC);
  lampSubscribe();
  if (SENSOR_HUM_TOPIC[0]) mqtt.subscribe(SENSOR_HUM_TOPIC);

  forcePublish = true;
  return true;
}

// ---------------------------------------------------------------
// Pamiec nieulotna (nastawa + tryb)
// ---------------------------------------------------------------
void persistLoad() {
  prefs.begin("thermo", false);
  float sp = prefs.getFloat("sp", 22.0f);
  int m = prefs.getUChar("mode", 0);
  setpoint = constrain(sp, TEMP_MIN, TEMP_MAX);
  mode = (Mode)constrain(m, 0, 2);
  savedSp = seenSp = setpoint;
  savedMode = seenMode = (int)mode;
}

void persistTick(uint32_t now) {
  if (fabsf(setpoint - seenSp) > 0.01f || (int)mode != seenMode) {
    seenSp = setpoint;
    seenMode = (int)mode;
    persistChangeMs = now;
  }
  // zapis dopiero 3 s po ostatniej zmianie (oszczedza pamiec flash)
  if ((fabsf(setpoint - savedSp) > 0.01f || (int)mode != savedMode) && now - persistChangeMs > 3000) {
    prefs.putFloat("sp", setpoint);
    prefs.putUChar("mode", (uint8_t)mode);
    savedSp = setpoint;
    savedMode = (int)mode;
    USBSerial.println("Zapisano nastawe i tryb");
  }
}

// ---------------------------------------------------------------
// Czujnik: brak odczytu -> grzanie zablokowane
// ---------------------------------------------------------------
void sensorWatchdog(uint32_t now) {
  if (SIMULATE_TEMPERATURE) return;
  bool stale = !gotSensor || (now - lastSensorMs > SENSOR_TIMEOUT_MS);
  if (stale != sensorStale) {
    sensorStale = stale;
    dirtyStatus = true;
    updateHeating();
  }
}

// ---------------------------------------------------------------
// Wywolywane z glownego pliku
// ---------------------------------------------------------------
void networkSetup() {
  snprintf(tSetTemp, sizeof(tSetTemp), "%s/set_temp", BASE_TOPIC);
  snprintf(tSetMode, sizeof(tSetMode), "%s/set_mode", BASE_TOPIC);
  snprintf(tSetPreset, sizeof(tSetPreset), "%s/set_preset", BASE_TOPIC);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_ID);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1280);
  mqtt.setSocketTimeout(2);
  mqtt.setKeepAlive(30);

  if (!SIMULATE_TEMPERATURE) {   // do pierwszego odczytu grzanie zablokowane
    sensorStale = true;
    dirtyStatus = true;
    updateHeating();
  }
}

void networkLoop() {
  uint32_t now = millis();
  static uint32_t lastMqttTry = 0;

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  if (wifiOk) {
    if (mqtt.connected()) {
      mqtt.loop();
    } else if (now - lastMqttTry > 10000 || lastMqttTry == 0) {
      lastMqttTry = now;
      mqttConnect();
    }
  }

  bool on = wifiOk && mqtt.connected();
  if (on != netOnline) {
    netOnline = on;
    dirtyStatus = true;
    USBSerial.println(on ? "Siec: online" : "Siec: offline");
  }
  if (on) publishChanges();

  sensorWatchdog(now);
  persistTick(now);
}
