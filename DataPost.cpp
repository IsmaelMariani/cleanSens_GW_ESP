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
#define DATAPOST_QUEUE_LEN         2        // 1 en vuelo + 1 recien encolada, alcanza de sobra a 1 TX/2min

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
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // GAS responde con 302 antes del resultado real
    http.addHeader("Content-Type", "application/json");

    int code = http.POST(json);
    String respBody = http.getString();
    http.end();

    if (code != 200) return false;
    // GAS siempre responde 200 (hasta con {"ok":false,"error":...} ante un
    // rechazo de validacion, ver procesarMedicion()) -- el codigo HTTP solo
    // confirma que el request llego, no que el dato se haya grabado.
    return respBody.indexOf("\"ok\":true") >= 0;
}

static void dataPostTask(void *param) {
    PostItem item;
    for (;;) {
        if (xQueueReceive(s_queue, &item, portMAX_DELAY) != pdTRUE) continue;

        bool wifiOk = (WiFi.status() == WL_CONNECTED);
        bool txOk   = wifiOk ? enviarInterno(item.readings, item.epochMedidoMs) : false;

        s_lastWifiOk = wifiOk;
        s_lastTxOk   = txOk;
        s_resultSeq++;
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
    // timeout 0 -- nunca bloquea Core 1. Si la cola esta llena (una TX
    // anterior todavia en vuelo) esta ronda se descarta.
    xQueueSend(s_queue, &item, 0);
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
