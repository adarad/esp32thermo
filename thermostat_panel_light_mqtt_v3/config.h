#pragma once
// ===== Ustawienia sieci - uzupelnij swoje dane =====

#define WIFI_SSID   "q2n"
#define WIFI_PASS   "adam4205"

#define MQTT_HOST   "192.168.88.129"   // adres brokera (np. add-on Mosquitto w Home Assistant)
#define MQTT_PORT   1883
#define MQTT_USER   "adam"      // "" jesli broker bez logowania
#define MQTT_PASS   "adam4205"

// Nazwa urzadzenia w Home Assistant i temat bazowy
#define DEVICE_ID      "thermostat_salon"
#define FRIENDLY_NAME  "Termostat Salon"
#define BASE_TOPIC     "thermostat/salon"

// --- Czujnik temperatury, z ktorego panel bierze pomiar ---
// Wariant A (domyslny): zwykla liczba, np. "21.7" na tym temacie.
//   Dla czujnika, ktory nie jest w MQTT, dodaj w HA automatyzacje publikujaca jego stan (przyklad w opisie).
// Wariant B: Zigbee2MQTT / JSON -> temat "zigbee2mqtt/nazwa_czujnika", a klucz ponizej = "temperature".
#define SENSOR_TEMP_TOPIC      "thermostat/salon/sensor_temp"
#define SENSOR_TEMP_JSON_KEY   ""            // "" = payload to sama liczba; "temperature" = JSON

// Wilgotnosc (opcjonalnie): "" = wylaczone
#define SENSOR_HUM_TOPIC       ""
#define SENSOR_HUM_JSON_KEY    ""            // np. "humidity"

// Po tylu ms bez odczytu panel uznaje czujnik za nieaktywny i blokuje grzanie
#define SENSOR_TIMEOUT_MS      (15UL * 60UL * 1000UL)
