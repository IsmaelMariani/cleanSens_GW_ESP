#include "PicLink.h"
#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// =============================================================
// PicLink.cpp -- ver PicLink.h para el diseño del protocolo.
//
// Usa Serial (UART0, GPIO1/GPIO3) -- el PIC esta conectado al conector de
// alimentacion de la placa (P1 en el ESP32-2432S028R/CYD), que es UART0, el
// mismo que el chip USB-serie de a bordo. Por esto no se puede usar
// Serial.print() para debug ni conectar el USB al mismo tiempo que el PIC.
// =============================================================

// 2026-07-31: sin fraccion/segundero/minuto (12->8 bytes) -- el nodo no manda
// su propio reloj en el uplink (ver nota grande en ra01.h del lado picg).
struct __attribute__((packed)) NodeRecordWire {
    uint8_t  id;
    uint8_t  addr;           // 0 = slot vacio esa ronda
    int16_t  presionMmH2O;   // centesimas de mmH2O, con signo -- clampeado a +-9999
    int8_t   tempC;
    int8_t   vbat;
    uint8_t  flags;
    uint8_t  reservado;
};

#define GW2ESP_ELAPSED_SENTINEL  0xFFFFu

#define FRAME_NODE_LEN    13   // header,cmd,NodeRecordWire(8),crc16(2),footer
#define FRAME_RESYNC_LEN   7   // header,cmd,elapsedLo,elapsedHi,flags,checksum,footer
#define FRAME_MAX_LEN     13

#define ACK_NODE_LEN       5   // header,cmd,addr,checksum,footer
#define ACK_RESYNC_LEN     7   // header,cmd,orden,fraccion,segundero,checksum,footer

// deadband -- por debajo de esto no se corrige OSCTUNE, evita "cazar"
// alrededor del punto optimo. Ver charla de diseño 2026-07-31.
#define ORDEN_DEADBAND_MS  30

static uint8_t rx[FRAME_MAX_LEN];
static uint8_t rxIdx        = 0;
static uint8_t rxExpectLen  = 0;   // 0 = todavia no se conoce (esperando el byte de cmd)
static unsigned long lastByteMs  = 0;
static unsigned long firstByteMs = 0;

#define BYTE_TIMEOUT_MS  150   // gap entre bytes -- generoso igual, frames chicos (<=13B) no deberian nunca acercarse
#define MAX_FRAME_MS     100   // racha entera demasiado lenta -- a 19200 baud, 13 bytes tardan ~7ms reales

static uint32_t g_framesOk       = 0;
static uint32_t g_discardTimeout = 0;
static uint32_t g_discardHeader  = 0;   // cmd invalido / footer no matchea
static uint32_t g_discardCrc     = 0;   // CRC16 (nodo) o checksum XOR (resync) invalido

static bool tocadoEstaRonda[NUM_NODES] = {false};

static unsigned long g_lastAckMs   = 0;
static bool           g_haveLastAck = false;
static int32_t        g_offsetMs    = 0;
static bool           g_haveOffset  = false;

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

static void convertNode(const NodeRecordWire &w, NodeReading &r) {
    r.presionMmH2O = w.presionMmH2O;   // ya en centesimas de mmH2O, el nodo lo manda convertido
    r.tempC        = w.tempC;          // ya en grados C, el nodo lo manda convertido
    r.vbat         = w.vbat;           // ya en decimas de volt, el nodo lo manda convertido
    r.idNodo       = w.id;
}

static void blankReading(NodeReading &r) {
    r.presionMmH2O = SENTINEL_INT16;
    r.tempC        = SENTINEL_INT8;
    r.vbat         = SENTINEL_INT8;
    r.idNodo       = SENTINEL_ID;
}

void picLink_init() {
    Serial.begin(19200, SERIAL_8N1);   // UART0 -- GPIO1/GPIO3 fijos, sin pines a elegir
    for (uint8_t i = 0; i < NUM_NODES; i++) tocadoEstaRonda[i] = false;
}

// -- procesa 1 frame ya completo en rx[0..rxIdx-1] (rxIdx==rxExpectLen) --
// valida, arma+manda el ACK correspondiente, actualiza readings[]/contadores.
// Devuelve el evento resultante (NONE si no valido).
static PicLinkEvent handleFrame(NodeReading readings[NUM_NODES], uint8_t fino, uint8_t segundero) {
    uint8_t cmd = rx[1];

    if (cmd == GW2ESP_CMD_NODE) {
        if (rx[FRAME_NODE_LEN - 1] != 0x55) { g_discardHeader++; return PicLinkEvent::NONE; }

        uint16_t crcCalc = crc16_ccitt(rx, FRAME_NODE_LEN - 3);   // header..nodo (offset 0..9)
        uint16_t crcWire = (uint16_t)rx[10] | ((uint16_t)rx[11] << 8);
        if (crcCalc != crcWire) { g_discardCrc++; return PicLinkEvent::NONE; }

        NodeRecordWire w;
        memcpy(&w, &rx[2], sizeof(NodeRecordWire));

        // ACK inmediato -- antes de tocar readings[], mismo criterio de
        // latencia minima que ya usaba el diseño anterior.
        uint8_t ack[ACK_NODE_LEN];
        ack[0] = 0xAA;
        ack[1] = GW2ESP_CMD_NODE;
        ack[2] = w.addr;
        ack[3] = (uint8_t)(ack[1] ^ ack[2]);
        ack[4] = 0x55;
        Serial.write(ack, sizeof(ack));

        if (w.addr >= 1 && w.addr <= NUM_NODES) {
            convertNode(w, readings[w.addr - 1]);
            tocadoEstaRonda[w.addr - 1] = true;
        }
        g_framesOk++;
        return PicLinkEvent::NODE;
    }

    if (cmd == GW2ESP_CMD_RESYNC) {
        if (rx[FRAME_RESYNC_LEN - 1] != 0x55) { g_discardHeader++; return PicLinkEvent::NONE; }

        uint8_t chk = (uint8_t)(rx[1] ^ rx[2] ^ rx[3] ^ rx[4]);
        if (chk != rx[5]) { g_discardCrc++; return PicLinkEvent::NONE; }

        uint16_t elapsedTicks = (uint16_t)rx[2] | ((uint16_t)rx[3] << 8);
        // rx[4] bit0 = hubo reset del picg desde el ultimo resync confirmado --
        // valor solo diagnostico por ahora (el caso "picg recien reseteo" ya
        // queda cubierto por el sentinel de elapsed, ver PicLink.h/gw2esp.h).

        unsigned long nowMs = millis();
        long deltaEspMs = g_haveLastAck ? (long)(nowMs - g_lastAckMs) : -1;

        uint8_t orden;
        if (elapsedTicks == GW2ESP_ELAPSED_SENTINEL || !g_haveLastAck) {
            orden = GW2ESP_ORDEN_NOTOCAR;
            g_haveOffset = false;
        } else {
            long picgMs   = (long)elapsedTicks * 10L;
            long offsetMs = deltaEspMs - picgMs;   // + : el picg corrio lento -> acelerar
            g_offsetMs   = (int32_t)offsetMs;
            g_haveOffset = true;
            if (labs(offsetMs) < ORDEN_DEADBAND_MS) {
                orden = GW2ESP_ORDEN_NOTOCAR;
            } else if (offsetMs >= 0) {
                orden = GW2ESP_ORDEN_INC;
            } else {
                orden = GW2ESP_ORDEN_DEC;
            }
        }
        g_lastAckMs   = nowMs;   // se pisa siempre que se manda un ack de resync, haya o no orden confiable
        g_haveLastAck = true;

        uint8_t ack[ACK_RESYNC_LEN];
        ack[0] = 0xAA;
        ack[1] = GW2ESP_CMD_RESYNC;
        ack[2] = orden;
        ack[3] = fino;
        ack[4] = segundero;
        ack[5] = (uint8_t)(ack[1] ^ ack[2] ^ ack[3] ^ ack[4]);
        ack[6] = 0x55;
        Serial.write(ack, sizeof(ack));

        // cierre de ronda -- blanquea las UTAs que no llegaron esta vuelta,
        // recien ahi resetea el marcador para la ronda que arranca. Este es
        // el unico punto donde readings[] pierde datos por ausencia -- un
        // relay que fallo sus 3 intentos, o un nodo que nunca reporto,
        // terminan aca, no antes.
        for (uint8_t i = 0; i < NUM_NODES; i++) {
            if (!tocadoEstaRonda[i]) blankReading(readings[i]);
            tocadoEstaRonda[i] = false;
        }

        g_framesOk++;
        return PicLinkEvent::ROUND_CLOSED;
    }

    g_discardHeader++;   // cmd desconocido -- no deberia pasar (ya se filtra al leer el byte de cmd)
    return PicLinkEvent::NONE;
}

PicLinkEvent picLink_poll(NodeReading readings[NUM_NODES], uint8_t fino, uint8_t segundero) {
    PicLinkEvent result = PicLinkEvent::NONE;

    while (Serial.available() && result == PicLinkEvent::NONE) {
        uint8_t b = (uint8_t)Serial.read();
        unsigned long now = millis();

        if (rxIdx == 0) {
            if (b != 0xAA) continue;   // busca header -- cualquier otra cosa se descarta en silencio
            rx[rxIdx++] = b;
            firstByteMs = now;
            lastByteMs  = now;
            continue;
        }

        if ((now - lastByteMs) > BYTE_TIMEOUT_MS) {
            // se corto la racha del frame en curso -- reinicia buscando
            // header de nuevo, este mismo byte puede ser el proximo 0xAA
            g_discardTimeout++;
            rxIdx = 0;
            if (b == 0xAA) { rx[rxIdx++] = b; firstByteMs = now; lastByteMs = now; }
            continue;
        }
        lastByteMs = now;

        if (rxIdx == 1) {
            // 2do byte -- el comando, define el largo esperado del frame
            if (b == GW2ESP_CMD_NODE) rxExpectLen = FRAME_NODE_LEN;
            else if (b == GW2ESP_CMD_RESYNC) rxExpectLen = FRAME_RESYNC_LEN;
            else {
                g_discardHeader++;
                rxIdx = 0;   // cmd invalido -- descarta, no reinterpreta este byte como header
                continue;
            }
            rx[rxIdx++] = b;
            continue;
        }

        rx[rxIdx++] = b;

        if (rxIdx >= rxExpectLen) {
            result = handleFrame(readings, fino, segundero);
            rxIdx       = 0;
            rxExpectLen = 0;
        }
    }

    // racha en curso que ya viene tardando demasiado en total (aunque cada
    // gap individual haya sido corto) -- mismo criterio que el diseño viejo
    if (rxIdx > 0 && (unsigned long)(millis() - firstByteMs) > MAX_FRAME_MS) {
        g_discardTimeout++;
        rxIdx       = 0;
        rxExpectLen = 0;
    }

    return result;
}

uint32_t picLink_framesOk()       { return g_framesOk; }
uint32_t picLink_discardTimeout() { return g_discardTimeout; }
uint32_t picLink_discardHeader()  { return g_discardHeader; }
uint32_t picLink_discardCrc()     { return g_discardCrc; }
int32_t  picLink_offsetMs()       { return g_haveOffset ? g_offsetMs : INT32_MIN; }
