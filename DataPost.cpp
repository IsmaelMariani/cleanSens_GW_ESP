// 2026-09-30 19:44 - Evento boot (motivo de reset + contador) a GAS
// 2026-09-30 19:23 - Reescrito: ventanas de radio, reintento, anillo
#include "DataPost.h"
#include "GasConfig.h"
#include "GatewayConfig.h"
#include "WifiPower.h"
#include "RealClock.h"
#include <time.h>
#include <esp_system.h>
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>
#include <stdlib.h>

// =============================================================
// DataPost.cpp -- ver DataPost.h para el diseño (cola, ventanas, reintentos).
// =============================================================

// stack grande a proposito -- WiFiClientSecure/mbedTLS (handshake TLS)
// necesita bastante mas que el default de una task chica.
#define DATAPOST_TASK_STACK_BYTES  10240
#define DATAPOST_TASK_CORE         0        // nunca Core 1 (loop()/picLink_poll/UART)

// 576 registros = ~6 dias a 15 min (~64KB de heap, contra ~280KB libres)
#define DATAPOST_QUEUE_LEN         576

#define DATAPOST_MAX_POR_VENTANA   8
#define DATAPOST_MAX_FALLOS_HTTP   5

#define TIPO_MEDICION  0
#define TIPO_EVENTO    1

struct PostItem {
    NodeReading readings[NUM_NODES];   // solo TIPO_MEDICION
    int64_t     epochMedidoMs;
    uint32_t    seq;          // identidad del registro (asignada al encolar)
    uint8_t     fallosHttp;   // cuantas veces GAS respondio algo que no es 302
    uint8_t     tipo;         // TIPO_MEDICION / TIPO_EVENTO
    uint8_t     resetReason;  // solo TIPO_EVENTO: esp_reset_reason_t
    uint32_t    reinicios;    // solo TIPO_EVENTO: contador de reinicios
};

// -- cola circular en heap, protegida por mutex (encolar corre en Core 1, la
// task de envio en Core 0; descartar el mas viejo al llenarse podria pisar el
// registro que esta en vuelo, por eso la baja se hace por seq, ver abajo) --
static PostItem         *s_ring  = nullptr;
static uint16_t          s_len   = 0;
static volatile uint16_t s_head  = 0;
static volatile uint16_t s_count = 0;
static uint32_t          s_nextSeq = 1;
static SemaphoreHandle_t s_mutex = nullptr;

// estado de la ventana actual
static volatile bool     s_ventanaAgotada   = false;
static volatile uint8_t  s_enviadosVentana  = 0;
static volatile bool     s_enVuelo          = false;

// resultado del ultimo intento (1 escritora: la task de Core 0)
static volatile bool     s_lastWifiOk = false;
static volatile bool     s_lastTxOk   = false;
static volatile uint32_t s_resultSeq  = 0;
static volatile int      s_lastHttpCode = -1;
static char              s_lastRespSnippet[120] = {0};

// contadores
static volatile uint32_t s_perdidos   = 0;
static volatile uint32_t s_entregados = 0;
static volatile uint32_t s_timeouts   = 0;
static volatile uint32_t s_ambiguos   = 0;

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
    // procesarMedicion()). "fyhMedido": null (no undefined) si no hay hora
    // real -- new Date(null) en GAS da 1970-01-01, un placeholder obvio de
    // detectar en el sheet, mejor que "Invalid Date".
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

static const char *motivoStr(uint8_t r) {
    switch ((esp_reset_reason_t)r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

// evento de arranque -- "fyh" = hora real al momento de ENVIAR menos el tiempo
// transcurrido desde el boot (millis()) = instante real del arranque, aunque el
// envio se haya demorado; null si todavia no hay hora real
static String armarJsonEvento(const PostItem &it) {
    String json = "{\"tipo\":\"evento\",\"ev\":\"boot\",\"fyh\":";
    if (realClock_valido()) {
        int64_t bootMs = (int64_t)time(nullptr) * 1000LL - (int64_t)millis();
        char buf[24];
        snprintf(buf, sizeof(buf), "%lld", (long long)bootMs);
        json += buf;
    } else {
        json += "null";
    }
    json += ",\"motivo\":\"";
    json += motivoStr(it.resetReason);
    json += "\",\"reinicios\":";
    json += String((unsigned long)it.reinicios);
    json += ",\"fw\":\"" GATEWAY_FW_TAG "\"}";
    return json;
}

// devuelve el codigo HTTP crudo (302 = GAS termino; negativo = error del
// cliente HTTP; -100 = http.begin() fallo)
static int enviarInterno(const PostItem &item) {
    String json = (item.tipo == TIPO_EVENTO) ? armarJsonEvento(item)
                                             : armarJson(item.readings, item.epochMedidoMs);

    WiFiClientSecure client;
    client.setInsecure();   // sin validar cert de Google, mismo criterio que WifiConfig.cpp

    HTTPClient http;
    if (!http.begin(client, GAS_WEBAPP_URL)) {
        s_lastHttpCode = -100;
        s_lastRespSnippet[0] = '\0';
        return -100;
    }
    // doPost() puede tardar hasta decenas de segundos en ejecutar (y mas, ver
    // DataPost.h) -- 20s de espera, si no llega el 302 a tiempo (-11) el dato
    // igual se graba del lado de GAS y se da por entregado.
    http.setTimeout(20000);
    // NO seguir el redirect: GAS responde 302 cuando el script termino sin
    // excepcion no capturada -- confirmado en banco que ese primer salto
    // siempre llega bien y el segundo (seguido como POST) devolvia HTTP 400.
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.addHeader("Content-Type", "application/json");

    int code = http.POST(json);
    String respBody = http.getString();
    http.end();

    s_lastHttpCode = code;
    strncpy(s_lastRespSnippet, respBody.c_str(), sizeof(s_lastRespSnippet) - 1);
    s_lastRespSnippet[sizeof(s_lastRespSnippet) - 1] = '\0';

    return code;
}

// -- operaciones de cola (todas bajo mutex) --

static bool peekHead(PostItem &out) {
    bool ok = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_count > 0) {
        out = s_ring[s_head];
        ok = true;
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

// saca el registro de la cola SOLO si sigue siendo la cabeza (si mientras
// estaba en vuelo la cola llena lo descarto, no se toca otro)
static void confirmarEntregado(uint32_t seq) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_count > 0 && s_ring[s_head].seq == seq) {
        s_head = (uint16_t)((s_head + 1) % s_len);
        s_count--;
    }
    xSemaphoreGive(s_mutex);
}

// GAS respondio algo que no es 302: cuenta el fallo; al tope lo descarta
static void marcarFalloHttp(uint32_t seq) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_count > 0 && s_ring[s_head].seq == seq) {
        s_ring[s_head].fallosHttp++;
        if (s_ring[s_head].fallosHttp >= DATAPOST_MAX_FALLOS_HTTP) {
            s_head = (uint16_t)((s_head + 1) % s_len);
            s_count--;
            s_perdidos++;
        }
    }
    xSemaphoreGive(s_mutex);
}

static void dataPostTask(void *param) {
    (void)param;
    PostItem item;

    for (;;) {
        // en modo de test aislado (sin WifiPower) no hay ventanas: se manda siempre que haya WiFi
        bool ventanaLibre = !wifiPower_iniciado() ||
                            (!s_ventanaAgotada && s_enviadosVentana < DATAPOST_MAX_POR_VENTANA);

        if (!wifiPower_enlaceListo() || !ventanaLibre || !peekHead(item)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (WiFi.status() != WL_CONNECTED) {   // el enlace se cayo con la ventana abierta
            s_lastWifiOk = false;
            s_lastTxOk   = false;
            s_resultSeq++;
            s_ventanaAgotada = true;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // evento de boot: espera (hasta 30 s tras el arranque) a que haya hora
        // real, para que "fyh" sea el instante real del boot
        if (item.tipo == TIPO_EVENTO && !realClock_valido() && millis() < 30000UL) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        s_enVuelo = true;
        int code = enviarInterno(item);
        s_enVuelo = false;

        s_lastWifiOk = true;

        if (code == 302 || code == HTTPC_ERROR_READ_TIMEOUT) {
            // dado por escrito -- ver DataPost.h
            confirmarEntregado(item.seq);
            s_enviadosVentana++;
            s_entregados++;
            if (code != 302) s_timeouts++;
            s_lastTxOk = true;
            DBG("[dp] entregado code=%d (pendientes=%u)\n", code, (unsigned)s_count);
        } else {
            s_lastTxOk = false;
            s_ventanaAgotada = true;   // no se manda nada mas en esta ventana
            if (code > 0) {
                marcarFalloHttp(item.seq);
            } else if (code == HTTPC_ERROR_SEND_PAYLOAD_FAILED || code == HTTPC_ERROR_CONNECTION_LOST) {
                s_ambiguos++;
            }
            DBG("[dp] NO entregado code=%d resp=%s\n", code, s_lastRespSnippet);
        }
        s_resultSeq++;
    }
}

void dataPost_init() {
    s_mutex = xSemaphoreCreateMutex();

    // anillo en heap -- si no hay un bloque contiguo del tamaño pedido, achica
    s_len = DATAPOST_QUEUE_LEN;
    while (s_len >= 16) {
        s_ring = (PostItem *)malloc(sizeof(PostItem) * s_len);
        if (s_ring) break;
        s_len /= 2;
    }
    if (!s_ring) s_len = 0;
    s_head = 0;
    s_count = 0;

    xTaskCreatePinnedToCore(dataPostTask, "dataPostTask",
                             DATAPOST_TASK_STACK_BYTES, nullptr, 1, nullptr,
                             DATAPOST_TASK_CORE);
}

void dataPost_encolar(const NodeReading readings[NUM_NODES], int64_t epochMedidoMs) {
    if (!s_ring || s_len == 0) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_count >= s_len) {
        // llena: se descarta el MAS VIEJO -- prioridad a conservar lo ultimo
        s_head = (uint16_t)((s_head + 1) % s_len);
        s_count--;
        s_perdidos++;
    }
    PostItem &dst = s_ring[(s_head + s_count) % s_len];
    memcpy(dst.readings, readings, sizeof(dst.readings));
    dst.epochMedidoMs = epochMedidoMs;
    dst.seq           = s_nextSeq++;
    dst.fallosHttp    = 0;
    dst.tipo          = TIPO_MEDICION;
    dst.resetReason   = 0;
    dst.reinicios     = 0;
    s_count++;
    xSemaphoreGive(s_mutex);
}

void dataPost_encolarBoot(uint8_t resetReason, uint32_t reinicios) {
    if (!s_ring || s_len == 0) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_count >= s_len) {
        s_head = (uint16_t)((s_head + 1) % s_len);
        s_count--;
        s_perdidos++;
    }
    PostItem &dst = s_ring[(s_head + s_count) % s_len];
    memset(dst.readings, 0, sizeof(dst.readings));
    dst.epochMedidoMs = 0;
    dst.seq           = s_nextSeq++;
    dst.fallosHttp    = 0;
    dst.tipo          = TIPO_EVENTO;
    dst.resetReason   = resetReason;
    dst.reinicios     = reinicios;
    s_count++;
    xSemaphoreGive(s_mutex);
}

void dataPost_nuevaVentana() {
    s_enviadosVentana = 0;
    s_ventanaAgotada  = false;
}

bool dataPost_enVuelo() {
    return s_enVuelo;
}

bool dataPost_inactivo() {
    if (s_enVuelo) return false;
    return s_count == 0 || s_ventanaAgotada || s_enviadosVentana >= DATAPOST_MAX_POR_VENTANA;
}

bool dataPost_ultimoOk()          { return s_lastTxOk; }
bool dataPost_ultimoWifiOk()      { return s_lastWifiOk; }
uint32_t dataPost_resultSeq()     { return s_resultSeq; }
int dataPost_ultimoCode()         { return s_lastHttpCode; }
const char *dataPost_ultimoResp() { return s_lastRespSnippet; }
uint16_t dataPost_pendientes()    { return s_count; }
uint32_t dataPost_perdidos()      { return s_perdidos; }
uint32_t dataPost_entregados()    { return s_entregados; }
uint32_t dataPost_timeouts()      { return s_timeouts; }
uint32_t dataPost_ambiguos()      { return s_ambiguos; }
