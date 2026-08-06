#include "WifiConfig.h"
#include "GasConfig.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>

// =============================================================
// WifiConfig.cpp -- ver WifiConfig.h para el diseño.
// =============================================================

// -- credencial de arranque: hotspot fijo del celu del usuario. Se usa para
// sembrar TANTO la activa como el fallback en el primer boot tras flashear
// (de ahi en mas el codigo nunca vuelve a leer estas constantes -- todo
// sale de flash).
#define SEMILLA_SSID  "masterwifi"
#define SEMILLA_PASS  "1q2w3e4r"

// -- token del doGet() de config remota -- string cualquiera, tiene que
// matchear el hardcodeado del lado GAS. Ver PresionDif_JIM/Config_WiFi para
// el layout de celdas (B2 ssid, B3 pass, B4 flag S/N).
#define WIFICFG_TOKEN    "1234"

#define WIFI_CONNECT_TIMEOUT_MS  8000   // por intento -- generoso para asociacion+DHCP
#define WIFI_CONNECT_ATTEMPTS    3      // solo para la credencial activa, ver wifiConfig_conectar()

static Preferences prefs;

void wifiConfig_init() {
    prefs.begin("wifi", false);
    if (!prefs.isKey("ssid")) {
        prefs.putString("ssid", SEMILLA_SSID);
        prefs.putString("pass", SEMILLA_PASS);
    }
    if (!prefs.isKey("ssidFb")) {
        prefs.putString("ssidFb", SEMILLA_SSID);
        prefs.putString("passFb", SEMILLA_PASS);
    }
}

static bool intentarConectar(const String &ssid, const String &pass, uint8_t intentos) {
    if (ssid.length() == 0) return false;
    for (uint8_t i = 0; i < intentos; i++) {
        WiFi.disconnect(true);
        // delay obligatorio -- WiFi.begin() llamado inmediatamente despues
        // de disconnect(true) sobre una conexion QUE YA ESTABA activa
        // (no en el primer boot, ahi el radio arranca limpio y esto es un
        // no-op) puede pisar el propio disconnect en curso y fallar en
        // silencio -- bug conocido del core ESP32-Arduino, confirmado en
        // banco 2026-08-05 (fallaba el primer ciclo de reconexion de
        // DataPost, el que corre con WiFi ya conectado desde el boot).
        delay(200);
        WiFi.begin(ssid.c_str(), pass.c_str());
        unsigned long t0 = millis();
        while ((unsigned long)(millis() - t0) < WIFI_CONNECT_TIMEOUT_MS) {
            if (WiFi.status() == WL_CONNECTED) return true;
            delay(100);
        }
    }
    return false;
}

void wifiConfig_conectar() {
    WiFi.mode(WIFI_STA);

    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    if (intentarConectar(ssid, pass, WIFI_CONNECT_ATTEMPTS)) return;

    // activa agoto sus intentos -- cae al fallback (celu), nunca tocado por
    // la actualizacion remota (wifiConfig_actualizarDesdeSheet() solo
    // escribe "ssid"/"pass", jamas "ssidFb"/"passFb")
    String fbSsid = prefs.getString("ssidFb", "");
    String fbPass = prefs.getString("passFb", "");
    intentarConectar(fbSsid, fbPass, 1);
}

bool wifiConfig_conectado() {
    return WiFi.status() == WL_CONNECTED;
}

String wifiConfig_ipStr() {
    if (!wifiConfig_conectado()) return "";
    return WiFi.localIP().toString();
}

// extraccion minima de un campo string de un JSON chico y de formato fijo
// (el que devuelve nuestro propio doGet(), ver WifiConfig.h) -- sin
// libreria JSON, no hace falta para 3 campos controlados por nosotros mismos.
static String extraerCampoJson(const String &json, const String &clave) {
    String buscado = "\"" + clave + "\":\"";
    int i = json.indexOf(buscado);
    if (i < 0) return "";
    i += buscado.length();
    int fin = json.indexOf('"', i);
    if (fin < 0) return "";
    return json.substring(i, fin);
}

bool wifiConfig_actualizarDesdeSheet() {
    if (!wifiConfig_conectado()) return false;

    WiFiClientSecure client;
    client.setInsecure();   // sin validar cert de Google -- el token en la URL es el control real (ver diseño 2026-08-03)

    HTTPClient http;
    String url = String(GAS_WEBAPP_URL) + "?token=" + WIFICFG_TOKEN;
    if (!http.begin(client, url)) return false;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // GAS responde con 302 antes del JSON real

    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();

    if (extraerCampoJson(body, "flag") != "S") return false;   // "N" o respuesta invalida -- no toca flash

    String ssid = extraerCampoJson(body, "ssid");
    String pass = extraerCampoJson(body, "pass");
    if (ssid.length() == 0) return false;

    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    return true;
}

String wifiConfig_activaStr() {
    return prefs.getString("ssid", "") + " / " + prefs.getString("pass", "");
}

String wifiConfig_fallbackStr() {
    return prefs.getString("ssidFb", "") + " / " + prefs.getString("passFb", "");
}

// orquesta la secuencia completa -- unico punto de entrada que hace falta
// llamar desde setup() (test aislado o produccion, ver .ino):
//   1) conecta (activa -> fallback si hace falta, wifiConfig_conectar())
//   2) chequea el sheet -- si el flag vino "N" (o fallo la consulta), listo,
//      se queda con la conexion que ya tiene.
//   3) si vino "S", la credencial activa en flash ya quedo actualizada --
//      la prueba YA (no espera al proximo boot), hasta WIFI_CONNECT_ATTEMPTS
//      veces.
//   4) si la nueva no prendio, vuelve al paso 1 (reconecta con la semilla) y
//      chequea el sheet de nuevo -- en la practica el flag ya deberia estar
//      en "N" del lado GAS (se consume en la lectura anterior), asi que esta
//      vuelta normalmente no actualiza nada y el ciclo corta solo. El tope
//      de abajo es defensa nomas, por si el reset del flag en GAS fallara y
//      siguiera devolviendo "S" (evita loop infinito real en el boot).
#define WIFICFG_BOOTSTRAP_MAX_CICLOS  3

void wifiConfig_bootstrap() {
    for (uint8_t ciclo = 0; ciclo < WIFICFG_BOOTSTRAP_MAX_CICLOS; ciclo++) {
        wifiConfig_conectar();

        if (!wifiConfig_actualizarDesdeSheet()) return;

        String ssid = prefs.getString("ssid", "");
        String pass = prefs.getString("pass", "");
        if (intentarConectar(ssid, pass, WIFI_CONNECT_ATTEMPTS)) return;
    }
}

#define WIFI_RECONNECT_INTERVAL_MS  30000   // cada cuanto reintenta si esta caido

void wifiConfig_mantener() {
    if (wifiConfig_conectado()) return;

    static unsigned long lastAttempt = 0;
    if ((unsigned long)(millis() - lastAttempt) < WIFI_RECONNECT_INTERVAL_MS) return;
    lastAttempt = millis();

    WiFi.reconnect();   // no bloqueante -- reusa la ultima credencial que WiFi.begin() probo
}
