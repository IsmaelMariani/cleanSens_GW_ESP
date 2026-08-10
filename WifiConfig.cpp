#include "WifiConfig.h"
#include "GasConfig.h"
#include "DataPost.h"
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

// 2026-08-10: cache en RAM de las 4 credenciales -- flash (Preferences/NVS)
// solo se toca en wifiConfig_init() (1 vez, boot) y al escribir una
// actualizacion nueva (wifiConfig_actualizarDesdeSheet()). Todo el resto del
// modulo (conectar/reconectar, incluido el reintento periodico de
// wifiConfig_mantener()) lee de aca, nunca de prefs directo -- un acceso a
// flash invalida la cache de instrucciones de AMBOS cores del ESP32 (misma
// flash fisica para datos y codigo), pudiendo stallear brevemente el Core 1
// justo cuando picLink_poll() esta a mitad de leer un frame del PICG por
// UART. Sospecha real de banco: reintentos de WiFi.begin() (que antes leian
// prefs.getString() en cada llamada) coincidiendo con perdida de frames del
// PICG -- ver [[project_presion_diferencial]].
static String s_ssid, s_pass, s_ssidFb, s_passFb;

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

    s_ssid   = prefs.getString("ssid", "");
    s_pass   = prefs.getString("pass", "");
    s_ssidFb = prefs.getString("ssidFb", "");
    s_passFb = prefs.getString("passFb", "");
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
    // no persistir a la NVS interna de WiFi en cada begin() -- ya tenemos
    // nuestra propia persistencia via Preferences/RAM (ver arriba), esta es
    // pura redundancia y significa una escritura a flash (stall de ambos
    // nucleos) por cada intento de conexion, incluidos los periodicos de
    // wifiConfig_mantener() durante un corte largo. 2026-08-10.
    WiFi.persistent(false);
    // 2026-08-06: reactivado para 2da parte de la prueba A/B -- con el gate
    // de 200ms del .ino ya puesto, esto aisla si el titileo/TX erratico
    // de hoy era por el modem-sleep en si o por leer RealClock demasiado
    // seguido. Si vuelve a titilar, el modem-sleep es la causa real y esta
    // linea se vuelve a sacar. Ver [[project_presion_diferencial]].
    WiFi.setSleep(true);

    // 2026-08-10: orden invertido a pedido del usuario -- celu PRIMERO (1
    // intento, rapido -- en campo nunca va a estar presente, no vale la pena
    // gastarle mas de 1 intento) y activa despues (3 intentos, la conexion
    // real esperada). El celu solo se usa en el momento de instalar (el
    // instalador esta ahi con el hotspot prendido); de ahi en mas el celu no
    // se vuelve a intentar nunca (ver wifiConfig_mantener()) -- si el
    // instalador necesita cambiar la credencial activa, la actualiza via
    // Config_WiFi/GAS (conectado por celu en ese momento) y resetea la
    // placa, no hace falta logica de "probar la nueva al toque".
    if (intentarConectar(s_ssidFb, s_passFb, 1)) return;

    intentarConectar(s_ssid, s_pass, WIFI_CONNECT_ATTEMPTS);
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
    s_ssid = ssid;   // cache en RAM al dia -- ver comentario grande junto a su declaracion
    s_pass = pass;
    return true;
}

String wifiConfig_activaStr() {
    return s_ssid + " / " + s_pass;
}

String wifiConfig_fallbackStr() {
    return s_ssidFb + " / " + s_passFb;
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

        if (intentarConectar(s_ssid, s_pass, WIFI_CONNECT_ATTEMPTS)) return;
    }
}

#define WIFI_RECONNECT_INTERVAL_MS  30000   // cada cuanto reintenta si esta caido

// 2026-08-06: reconexion forzada -- DataPost.cpp (Core0) puede detectar que
// WiFi.status() dice "conectado" pero no hay internet real (router colgado,
// asociacion WiFi viva) y pedir (nunca ejecutar) que se fuerce una
// reconexion aca, el unico lugar que toca el WiFi. Maquina de 2 pasos
// repartida en varias vueltas de loop() (nunca un delay() bloqueante):
// WiFi.disconnect() en esta vuelta, WiFi.reconnect() recien pasados los
// ~200ms del bug ya conocido (ver intentarConectar()) en una vuelta
// posterior -- sin bloquear Core1/picLink_poll() en ningun momento.
enum ForzarReconexionState { FR_IDLE, FR_ESPERANDO_RECONECTAR };
static ForzarReconexionState frState        = FR_IDLE;
static unsigned long         frDisconnectMs = 0;

void wifiConfig_mantener() {
    if (frState == FR_ESPERANDO_RECONECTAR) {
        if ((unsigned long)(millis() - frDisconnectMs) >= 200) {
            WiFi.reconnect();
            frState = FR_IDLE;
        }
        return;   // nada mas esta vuelta -- ni chequeo de "conectado" ni timer normal
    }

    if (dataPost_debeForzarReconexion()) {
        WiFi.disconnect(true);
        frDisconnectMs = millis();
        frState        = FR_ESPERANDO_RECONECTAR;
        return;
    }

    if (wifiConfig_conectado()) return;

    static unsigned long lastAttempt = 0;
    if ((unsigned long)(millis() - lastAttempt) < WIFI_RECONNECT_INTERVAL_MS) return;
    lastAttempt = millis();

    // 2026-08-10: SIEMPRE activa, nunca celu -- a diferencia de
    // WiFi.reconnect() (que reintenta la ultima credencial que se probo, y
    // si el boot conecto por celu -- instalacion -- quedaria reintentando
    // celu para siempre tras una caida posterior), esto apunta directo a la
    // credencial guardada pase lo que pase. El celu es solo para el momento
    // de instalar (ver wifiConfig_conectar()), nunca para reconexion en
    // operacion. WiFi.begin() no bloquea (a diferencia de intentarConectar()).
    if (s_ssid.length() == 0) return;
    WiFi.begin(s_ssid.c_str(), s_pass.c_str());
}
