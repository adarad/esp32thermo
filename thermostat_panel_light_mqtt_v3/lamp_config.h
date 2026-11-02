#pragma once
// ===== Lampa (cieple + zimne LED) - sterowanie bezposrednie przez MQTT =====
//
// Panel i lampa rozmawiaja prostymi tematami. Payloady to zwykle teksty / liczby.
//
//   KOMENDY (panel -> lampa, bez retain):
//     LAMP_BASE/power/set        ON | OFF
//     LAMP_BASE/brightness/set   0..100        (jasnosc w procentach)
//     LAMP_BASE/color/set        0..100        (0 = same cieple LED, 100 = same zimne LED)
//
//   STANY (lampa -> panel, zalecane z retain):
//     LAMP_BASE/power            ON | OFF
//     LAMP_BASE/brightness       0..100
//     LAMP_BASE/color            0..100
//
// Panel dziala tez, gdy lampa nie publikuje stanow (wtedy pokazuje to, co sam ustawil).
// Mapowanie na kanaly LED robisz w firmware lampy, np.:
//   cieple = brightness * (100 - color) / 100
//   zimne  = brightness * color / 100

#define LAMP_NAME  "SWIATLO"        // naglowek strony (bez polskich znakow - wbudowana czcionka)
#define LAMP_BASE  "lamp/salon"     // temat bazowy lampy
