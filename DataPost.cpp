#include "DataPost.h"
#include "GasConfig.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <string.h>

// =============================================================
// DataPost.cpp -- ver DataPost.h para el diseño.
//
// 2026-08-05: el POST (http.POST(), bloqueante, puede tardar varios
// segundos) corre en una task de FreeRTOS pinneada a Core 0 -- nunca en el
// loop() de Core 1, que es donde vive picLink_poll()/fino/segundero (el
// timing con el PICG por UART es vital). A diferencia del primer intento de
// este mismo diseño (mismo dia, descartado): esta task NO TOCA el WiFi para
// nada -- no conecta, no desconecta, no cambia de modo. Solo mira
// WiFi.status() y, si ya esta conectado, manda. La conexion/reconexion
// (wifiConfig_conectar()/wifiConfig_mantener()) sigue siendo responsabilidad
// exclusiva de loop()/Core 1 -- separar del todo "quien mantiene la
// conexion" de "quien manda el dato" evito repetir el bug de reconexion que
// tuvo el intento anterior.
// =============================================================

// stack grande a proposito -- WiFiClientSecure/mbedTLS (handshake TLS)
// necesita bastante mas que el default de una task chica.
#define DATAPOST_TASK_STACK_BYTES  10240
#define DATAPOST_TASK_CORE         0        // nunca Core 1 (loop()/picLink_poll/UART)

// 2026-08-06: buffer de cortes de internet -- antes 2 (1 en vuelo + 1
// recien encolada), ahora dimensionado para no perder datos durante un
// corte largo. 576 = 2 dias / 5min (el intervalo minimo que va a pedir el
// cliente, ver Averager.h -- dimensionar contra el minimo es el caso
// conservador: con un intervalo mayor, estos mismos registros cubren MAS
// de 2 dias, nunca menos). PostItem pesa ~100 bytes -> ~58KB total, contra
// ~280KB de heap libre medidos en banco en operacion normal (WiFi+display+
// esta misma task activos) -- deja margen de sobra para todo lo que falta
// implementar (sistema de alertas, etc.).
#define DATAPOST_QUEUE_LEN         576

// cada cuanto reintenta el envio mas viejo si fallo -- mismo ritmo que
// wifiConfig_mantener() (no tiene sentido reintentar mas seguido que lo
// que la propia capa de WiFi intenta reconectar).
#define DATAPOST_RETRY_DELAY_MS    30000

// cada cuantos fallos SEGUIDOS (con WiFi.status() diciendo "conectado") se
// chequea conectividad real -- ver hayInternetReal()/s_forzarReconexion mas
// abajo.
#define DATAPOST_CONECTIVIDAD_CADA_N_FALLOS  3

struct PostItem {
    NodeReading readings[NUM_NODES];
    int64_t     epochMedidoMs;
};

static QueueHandle_t s_queue = nullptr;

// resultado del ultimo ciclo -- 1 sola escritora (la task de Core 0), 1 sola
// lectora (loop() en Core 1). bools de 1 byte: lectura/escritura atomica en
// la practica, no hace falta mutex para flags asi. wifiOk separado de txOk
// -- 2 fallas distintas a distinguir en pantalla (no habia conexion vs.
// conecto pero el POST/GAS fallo). s_resultSeq incrementa 1x por ciclo
// completo, para que loop() detecte "hay resultado nuevo" sin comparar
// booleans.
static volatile bool     s_lastWifiOk = false;
static volatile bool     s_lastTxOk   = false;
static volatile uint32_t s_resultSeq  = 0;

// 2026-08-07: cuantos envios se descartaron tras fallar (ver dataPostTask()
// mas abajo) -- contador interno, sin mostrar en pantalla todavia, para
// medir cuan seguido pasa esto antes de decidir si hace falta logica de
// verificacion contra tipo=ultima.
static volatile uint32_t s_perdidos = 0;

// diagnostico 2026-08-06 -- code crudo + fragmento del body de la ultima
// respuesta HTTP, mismo criterio de "unica escritora/unica lectora" que los
// bools de arriba. code=-1 si http.begin() fallo o si no hubo intento
// (WiFi no conectado). Se muestran en pantalla cuando la TX falla, ver
// DisplayManager::showPostFail() / .ino.
static volatile int s_lastHttpCode = -1;
static char         s_lastRespSnippet[120] = {0};

// 2026-08-06 -- pedido de reconexion forzada, Core0 (esta task) -> Core1
// (wifiConfig_mantener()). Escritora: esta task, cuando WiFi.status() dice
// "conectado" pero ni siquiera resuelve DNS (router colgado con la
// asociacion WiFi todavia viva -- WiFi.status() no lo detecta solo).
// Lectora+consumidora: wifiConfig_mantener(), que la pone en false apenas
// la lee (dataPost_debeForzarReconexion()). Esta task NUNCA toca el WiFi
// directamente -- solo pide, Core1 decide y ejecuta (ver DataPost.h).
static volatile bool s_forzarReconexion = false;

// resolucion DNS contra un host externo conocido -- liviano (no arma TLS ni
// hace un POST completo), sirve para distinguir "WiFi asociado pero sin
// salida real a internet" (router colgado) de "efectivamente sin WiFi"
// (ya cubierto por wifiOk==false, camino normal de wifiConfig_mantener()).
static bool hayInternetReal() {
    IPAddress ip;
    return WiFi.hostByName("www.google.com", ip);
}

static void agregarArrayInt(String &json, const char *clave,
                             const NodeReading readings[NUM_NODES],
                             int32_t (*extraer)(const NodeReading &)) {
    json += "\"";
    json += clave;
    json += "\":[";
    for (uint8_t i = 0; i < NUM_NODES; i++) {
        if (i) json += ",";
        json += readings[i].valid() ? String(extraer(readings[i])) : String("null");
    }
    json += "]";
}

static int32_t campoN(const NodeReading &r) { return r.idNodo; }
static int32_t campoP(const NodeReading &r) { return r.presionMmH2O; }
static int32_t campoT(const NodeReading &r) { return r.tempC; }
static int32_t campoV(const NodeReading &r) { return r.vbat; }

static String armarJson(const NodeReading readings[NUM_NODES], int64_t epochMedidoMs) {
    // "tipo" obligatorio -- lo exige doPost() del lado GAS (ver
    // procesarMedicion()). "fyhMedido": null (no undefined) si todavia no
    // hay sync NTP -- new Date(null) en GAS da 1970-01-01, un placeholder
    // obvio de detectar en el sheet, mejor que "Invalid Date" (lo que daria
    // omitir el campo).
    String json = "{\"tipo\":\"medicion\",\"fyhMedido\":";
    if (epochMedidoMs > 0) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%lld", (long long)epochMedidoMs);
        json += buf;
    } else {
        json += "null";
    }
    json += ",";
    agregarArrayInt(json, "n", readings, campoN); json += ",";
    agregarArrayInt(json, "p", readings, campoP); json += ",";
    agregarArrayInt(json, "t", readings, campoT); json += ",";
    agregarArrayInt(json, "v", readings, campoV);
    json += "}";
    return json;
}

static bool enviarInterno(const NodeReading readings[NUM_NODES], int64_t epochMedidoMs) {
    String json = armarJson(readings, epochMedidoMs);

    WiFiClientSecure client;
    client.setInsecure();   // sin validar cert de Google, mismo criterio que WifiConfig.cpp

    HTTPClient http;
    if (!http.begin(client, GAS_WEBAPP_URL)) return false;
    // default (~5000ms) insuficiente -- confirmado en banco (-11 =
    // HTTPC_ERROR_READ_TIMEOUT): doPost() puede tardar hasta 13s en
    // ejecutar (Log de Ejecuciones de Apps Script, 2026-08-06), y ese tiempo
    // va antes del 302 -- 20s da margen aunque las hojas sigan creciendo.
    http.setTimeout(20000);
    // 2026-08-06: NO seguir el redirect. GAS siempre responde 302 (Location a
    // script.googleusercontent.com) cuando el script termino sin excepcion no
    // capturada -- confirmado que ese primer salto SIEMPRE llega bien (datos
    // grabados, Log de Ejecuciones en "Completada" en todos los casos
    // probados). El segundo salto (persiguiendo el contenido real via
    // HTTPC_STRICT_FOLLOW_REDIRECTS) devolvia sistematicamente HTTP 400 +
    // pagina de error HTML de Google, nunca el JSON real -- causa exacta de
    // por que la "I" quedaba siempre roja pese a que el dato llegaba. Como
    // los 4 arrays del firmware siempre tienen 15 elementos por construccion,
    // el unico caso de {"ok":false} posible del lado de procesarMedicion()
    // nunca ocurre en la practica -- code==302 es un proxy confiable de
    // exito, sin necesidad de perseguir el contenido.
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.addHeader("Content-Type", "application/json");

    int code = http.POST(json);
    String respBody = http.getString();
    http.end();

    s_lastHttpCode = code;
    strncpy(s_lastRespSnippet, respBody.c_str(), sizeof(s_lastRespSnippet) - 1);
    s_lastRespSnippet[sizeof(s_lastRespSnippet) - 1] = '\0';

    return code == 302;
}

static void dataPostTask(void *param) {
    PostItem item;
    bool    havePending    = false;   // true = "item" es un envio que ya fallo antes, reintentar el MISMO
    uint8_t fallosSeguidos = 0;
    for (;;) {
        if (!havePending) {
            if (xQueueReceive(s_queue, &item, portMAX_DELAY) != pdTRUE) continue;
        }

        bool wifiOk = (WiFi.status() == WL_CONNECTED);
        bool txOk   = wifiOk ? enviarInterno(item.readings, item.epochMedidoMs) : false;

        s_lastWifiOk = wifiOk;
        s_lastTxOk   = txOk;
        s_resultSeq++;

        if (txOk) {
            havePending    = false;   // se mando -- la proxima vuelta saca el siguiente de la cola
            fallosSeguidos = 0;
        } else {
            // 2026-08-07: ya no se reintenta el mismo item -- se descarta y
            // se cuenta como perdido. Antes se reintentaba cada
            // DATAPOST_RETRY_DELAY_MS indefinidamente, pero si GAS tardaba
            // mas que el timeout del cliente (140s vistos en banco, contra
            // 20s de http.setTimeout()) igual terminaba escribiendo la fila
            // -- el reintento generaba una segunda ejecucion de doPost() que
            // tambien la escribia, duplicando la fila en Lecturas/Ledger.
            // Por ahora se acepta perder esa ronda puntual para medir cuan
            // seguido pasa esto antes de decidir si hace falta logica de
            // verificacion contra tipo=ultima.
            havePending = false;
            s_perdidos++;
            fallosSeguidos++;

            // WiFi.status() dice "conectado" pero la TX igual fallo varias
            // veces seguidas -- puede ser el router colgado (asociacion WiFi
            // viva, sin salida real a internet). Chequeo liviano de DNS; si
            // tampoco resuelve, pide a Core1 que fuerce una reconexion
            // (nunca la ejecuta esta misma task, ver s_forzarReconexion).
            if (wifiOk && (fallosSeguidos % DATAPOST_CONECTIVIDAD_CADA_N_FALLOS) == 0) {
                if (!hayInternetReal()) s_forzarReconexion = true;
            }

            vTaskDelay(pdMS_TO_TICKS(DATAPOST_RETRY_DELAY_MS));
        }
    }
}

void dataPost_init() {
    s_queue = xQueueCreate(DATAPOST_QUEUE_LEN, sizeof(PostItem));
    xTaskCreatePinnedToCore(dataPostTask, "dataPostTask",
                             DATAPOST_TASK_STACK_BYTES, nullptr, 1, nullptr,
                             DATAPOST_TASK_CORE);
}

void dataPost_encolar(const NodeReading readings[NUM_NODES], int64_t epochMedidoMs) {
    if (!s_queue) return;
    PostItem item;
    memcpy(item.readings, readings, sizeof(item.readings));
    item.epochMedidoMs = epochMedidoMs;
    // timeout 0 -- nunca bloquea Core 1. Buffer circular: si esta llena
    // (corte de internet mas largo que DATAPOST_QUEUE_LEN*intervalo), se
    // descarta el registro MAS VIEJO para hacer lugar al nuevo -- prioridad
    // a conservar siempre los ultimos ~2 dias, no los primeros del corte.
    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        PostItem descartado;
        xQueueReceive(s_queue, &descartado, 0);   // saca el mas viejo (no bloqueante)
        xQueueSend(s_queue, &item, 0);            // ahora hay lugar
    }
}

bool dataPost_ultimoOk() {
    return s_lastTxOk;
}

bool dataPost_ultimoWifiOk() {
    return s_lastWifiOk;
}

uint32_t dataPost_resultSeq() {
    return s_resultSeq;
}

int dataPost_ultimoCode() {
    return s_lastHttpCode;
}

const char *dataPost_ultimoResp() {
    return s_lastRespSnippet;
}

uint16_t dataPost_pendientes() {
    if (!s_queue) return 0;
    return (uint16_t)uxQueueMessagesWaiting(s_queue);
}

uint32_t dataPost_perdidos() {
    return s_perdidos;
}

bool dataPost_debeForzarReconexion() {
    if (s_forzarReconexion) {
        s_forzarReconexion = false;   // consumido -- se lee 1 sola vez
        return true;
    }
    return false;
}
