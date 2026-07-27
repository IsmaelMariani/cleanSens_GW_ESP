#include "PicLink.h"
#include <Arduino.h>
#include <string.h>
#include <stddef.h>

// =============================================================
// PicLink.cpp
//
// Formato del frame tal cual lo arma gw2esp.c del lado PIC -- mismo
// layout byte a byte (LSB primero en los campos multi-byte, PIC y
// ESP32 son ambos little-endian, no hace falta swap). Structs
// "wire" separados de NodeReading (el modelo que usa la UI) -- la
// conversion/escala pasa toda por convertNode().
//
// Usa Serial (UART0, GPIO1/GPIO3) en vez de Serial2 -- el PIC esta
// conectado al conector de alimentacion de la placa (P1 en el
// ESP32-2432S028R/CYD), que es UART0, el mismo que el chip USB-serie de
// a bordo. Por esto MISMO no se puede usar Serial.print() para debug
// (inyectaria bytes en medio del protocolo binario del PIC) ni conectar
// el USB al mismo tiempo que el PIC (contencion con el chip USB-serie).
// El debugging de esta etapa es 100% visual, por el volcado hex en
// pantalla (ver .ino, DEBUG_SHOW_RAW_HEX).
// =============================================================

struct __attribute__((packed)) NodeRecordWire {
    uint8_t  id;
    uint8_t  addr;
    uint16_t presionRaw16;   // TEMPORAL: crudo del ABP2, sin convertir -- ver SharedTypes.h
    int8_t   tempC;          // grados C -- unidad final, ya convertida en el nodo
    int8_t   vbat;           // decimas de volt (ej. 28 = 2,8V) -- unidad final, ya convertida en el nodo
    uint8_t  fraccion;       // 0-99, centesimas dentro del segundo
    uint8_t  segundero;      // 0-59
    uint16_t minuto;         // 0-1439, minuto del dia
    uint8_t  flags;
    uint8_t  reservado;
};

struct __attribute__((packed)) Gw2EspFrameWire {
    uint8_t         header;
    uint8_t         gatewayId;
    uint8_t         gatewayAddr;
    NodeRecordWire  nodo;      // 1 nodo (addr 1..15) o el latido (addr==0)
    uint16_t        crc16;
    uint8_t         footer;
};

// ACK ESP32->gateway -- mismo layout que esp2gw_ack_t en gw2esp.c del PIC
struct __attribute__((packed)) Esp2GwAckWire {
    uint8_t  header;
    uint8_t  fraccion;    // 0-99
    uint8_t  segundero;   // 0-59
    uint16_t minuto;
    uint16_t crc16;
    uint8_t  footer;
};

#define FRAME_SIZE  (sizeof(Gw2EspFrameWire))   // 18 bytes

static uint8_t buf[FRAME_SIZE];
static size_t  bufLen = 0;
static unsigned long lastByteMs  = 0;   // ultimo byte leido -- para detectar gap entre bytes
static unsigned long firstByteMs = 0;   // primer byte de la racha actual -- para detectar
                                        // que la racha entera tarda demasiado

// contadores de debugging -- se muestran en la barra de estado del display
// (no hay Monitor Serie disponible en banco sin el USB conectado)
static uint32_t g_bytesTotal      = 0;   // total de bytes recibidos alguna vez
                                          // (si queda en 0, es problema de cableado/baudrate,
                                          // no de framing/CRC)
static uint32_t g_framesOk       = 0;
static uint32_t g_discardTimeout = 0;   // se corto la racha antes de completar 186 bytes
static uint32_t g_discardHeader  = 0;   // se completaron 186 bytes pero header/footer no matchea
static uint32_t g_discardCrc     = 0;   // header/footer ok pero el CRC no matchea

static unsigned long g_lastFrameOkMs   = 0;   // millis() del ultimo frame validado OK
static uint32_t       g_frameIntervalMs = 0;   // ms entre los 2 ultimos frames OK -- 0 hasta el 2do

// snapshot del ultimo intento de 186 bytes completado, sea cual sea el
// resultado de su validacion -- para volcado hex en el display
static uint8_t  lastRawFrame[FRAME_SIZE];
static bool     hasLastRawFrame = false;
static uint32_t g_captureSeq    = 0;

// si se corta la racha de bytes a mitad de frame (ruido, corte de energia
// del lado PIC, etc.), descartar el parcial -- evita que bytes viejos se
// mezclen con el proximo frame real (mismo tipo de bug que encontramos en
// gw2esp_waitAck() del lado PIC por no limpiar el buffer entre intentos)
#define BYTE_TIMEOUT_MS  200

// a 19200 baud, 186 bytes reales (una rafaga continua del PIC) tardan
// ~97ms. Si la racha entera tarda mucho mas que eso en completarse, no es
// una rafaga real -- puede ser ruido intermitente que nunca dispara el
// gap de BYTE_TIMEOUT_MS (cada gap individual queda corto) pero igual
// termina rellenando el buffer de a poquito con basura mezclada.
#define MAX_FRAME_MS  300

static uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// llamar solo con w.addr en 1..NUM_NODES (nodo real) -- el latido (addr==0)
// no pasa por aca, se maneja aparte en picLink_poll()
static void convertNode(const NodeRecordWire &w, NodeReading &r) {
    r.presionRaw16 = w.presionRaw16;   // crudo, sin escalar -- la UI lo muestra en hex
    r.tempC        = w.tempC;          // ya en grados C, el nodo lo manda convertido
    r.vbat         = w.vbat;           // ya en decimas de volt, el nodo lo manda convertido
    r.fino         = w.fraccion;       // ya viene separado del wire, sin modulo
    r.segundero    = w.segundero;
    r.minutero     = w.minuto;
}

void picLink_init() {
    Serial.begin(19200, SERIAL_8N1);   // UART0 -- GPIO1/GPIO3 fijos, sin pines a elegir
}

PicLinkEvent picLink_poll(NodeReading readings[NUM_NODES], uint8_t *gatewayId, uint8_t *gatewayAddr) {
    // primero drenar lo que ya llego -- los bytes esperan tranquilos en el
    // buffer de Serial aunque el loop() se haya demorado en volver a
    // llamar esta funcion (ej. durante un redibujo de pantalla). Si el
    // timeout se chequeara antes de drenar, un simple retraso de NUESTRO
    // lado (no una perdida real de datos) tiraria el frame a la basura.
    while (Serial.available() && bufLen < FRAME_SIZE) {
        if (bufLen == 0) firstByteMs = millis();   // marca el arranque de esta racha
        buf[bufLen++] = (uint8_t)Serial.read();
        lastByteMs = millis();
        g_bytesTotal++;
    }

    // reset solo si de verdad se corto la racha -- por gap entre bytes
    // (nada nuevo en BYTE_TIMEOUT_MS) o porque la racha entera ya viene
    // tardando mas de lo que puede tardar una rafaga real (MAX_FRAME_MS)
    if (bufLen > 0 && bufLen < FRAME_SIZE) {
        unsigned long now = millis();
        bool gapLargo    = (now - lastByteMs)  > BYTE_TIMEOUT_MS;
        bool rachaLenta  = (now - firstByteMs) > MAX_FRAME_MS;
        if (gapLargo || rachaLenta) {
            g_discardTimeout++;
            bufLen = 0;
        }
    }

    if (bufLen < FRAME_SIZE) return PicLinkEvent::NONE;

    bufLen = 0;   // buffer libre para el proximo frame, se valide bien o no este

    memcpy(lastRawFrame, buf, FRAME_SIZE);   // snapshot para volcado hex, pase o no la validacion
    hasLastRawFrame = true;
    g_captureSeq++;

    Gw2EspFrameWire frame;
    memcpy(&frame, buf, FRAME_SIZE);

    if (frame.header != 0xAA || frame.footer != 0x55) {
        g_discardHeader++;
        return PicLinkEvent::NONE;
    }

    uint16_t crcCalc = crc16_ccitt(buf, offsetof(Gw2EspFrameWire, crc16));
    if (crcCalc != frame.crc16) {
        g_discardCrc++;
        return PicLinkEvent::NONE;
    }

    *gatewayId   = frame.gatewayId;
    *gatewayAddr = frame.gatewayAddr;
    g_framesOk++;

    if (frame.nodo.addr == 0) {
        // intervalo medido SOLO entre latidos (1/ronda, ~60s) -- no entre
        // cada frame de nodo individual (~1seg), para seguir viendo el
        // mismo diagnostico de desincronizacion de ronda que antes
        unsigned long nowMs = millis();
        if (g_lastFrameOkMs != 0) g_frameIntervalMs = (uint32_t)(nowMs - g_lastFrameOkMs);
        g_lastFrameOkMs = nowMs;
        return PicLinkEvent::HEARTBEAT;   // el llamador pasa readings[] a la UI y lo resetea
    }
    if (frame.nodo.addr <= NUM_NODES) {
        convertNode(frame.nodo, readings[frame.nodo.addr - 1]);
        return PicLinkEvent::NODE;
    }
    return PicLinkEvent::NONE;   // addr fuera de rango -- no deberia pasar con CRC ok
}

void picLink_sendAck(uint8_t fraccion, uint8_t segundero, uint16_t minuto) {
    Esp2GwAckWire ack;
    ack.header    = 0xAA;
    ack.fraccion  = fraccion;
    ack.segundero = segundero;
    ack.minuto    = minuto;
    ack.footer    = 0x55;
    ack.crc16     = crc16_ccitt((uint8_t *)&ack, offsetof(Esp2GwAckWire, crc16));

    Serial.write((uint8_t *)&ack, sizeof(ack));
}

uint32_t picLink_bytesTotal()     { return g_bytesTotal; }
uint32_t picLink_framesOk()       { return g_framesOk; }
uint32_t picLink_discardTimeout() { return g_discardTimeout; }
uint32_t picLink_discardHeader()  { return g_discardHeader; }
uint32_t picLink_discardCrc()     { return g_discardCrc; }
uint32_t picLink_frameIntervalMs() { return g_frameIntervalMs; }
size_t   picLink_rawFrameSize()   { return FRAME_SIZE; }
uint32_t picLink_captureSeq()     { return g_captureSeq; }

bool picLink_getLastRawFrame(uint8_t *out, size_t outSize) {
    if (!hasLastRawFrame || outSize < FRAME_SIZE) return false;
    memcpy(out, lastRawFrame, FRAME_SIZE);
    return true;
}

size_t picLink_currentBufLen() { return bufLen; }
