#include <Arduino.h>
#include <math.h>
#include "gateway_pinout.h"
#include "SharedTypes.h"
#include "Display.h"
#include "PicLink.h"

// =============================================================
// cleanSensGateway_v001.ino
// Etapa 2: UI + recepcion real de la UART del PIC gateway (sin ACK
// de vuelta ni WiFi/POST todavia -- esta etapa es solo para validar
// que el frame llega bien y ver los valores reales en pantalla).
// =============================================================

// TEMPORAL: diagnostico maximo del salto de tiempo en la ronda del gateway
// -- el PIC-g (main.c) quedo reducido a SOLO avanzar el timer y mandar 1
// byte crudo 0xAA (sin protocolo/CRC/ACK) cada vez que pasa por
// fraccion=0/segundero=50. Aca solo medimos ms entre 0xAA sucesivos y los
// mostramos en pantalla, renglon por renglon. Tiene prioridad sobre
// DEBUG_SHOW_RAW_HEX/DEBUG_RAW_BYTES si mas de uno esta en 1.
#define DEBUG_TIMING_ONLY  1

// TEMPORAL: debug de bajo nivel del UART PIC->ESP32 -- bytesTotal:0 y
// contadores O/I/E en 0 pese a confirmar con osciloscopio y continuidad
// a tester que el PIC transmite y llega al pin correcto del ESP32.
// Volver ambos a 0 una vez resuelto.
#define DEBUG_SHOW_RAW_HEX  0

// Nivel de debug mas simple todavia que DEBUG_SHOW_RAW_HEX -- lee Serial
// directo, sin pasar por PicLink (framing/CRC/timeouts), solo cuenta
// bytes. Descarta de una que el problema este en la logica de framing de
// PicLink.cpp. Tiene prioridad sobre DEBUG_SHOW_RAW_HEX si ambos estan en 1.
#define DEBUG_RAW_BYTES  0

DisplayManager display;
NodeReading    readings[NUM_NODES];
uint8_t        lastGatewayId   = 0;
uint8_t        lastGatewayAddr = 0;

// -- reloj: fecha fija ficticia (WiFi/NTP pendiente), pero el avance de
// hora/minuto es autonomo del ESP32 -- mismo patron que synctime/minuto
// del lado PIC (ver synctime.c/minuto.h), sin relacion con los frames
// recibidos: fino (10ms) -> segundero (1s) -> minutero (0..1439, minuto
// del dia). Cuando haya NTP, alcanza con pisar minutero (y segundero/fino
// si se quiere el segundo exacto), igual criterio que minuto_set() del PIC.
uint8_t  simDay = 20, simMonth = 7, simYear = 26;

#define MINUTERO_MAX  1440   // minutos por dia

static uint8_t  fino      = 0;     // 0..99, centesimas dentro del segundo actual
static uint8_t  segundero = 0;     // 0..59
static uint16_t minutero  = 600;   // 0..1439 -- arranca en 10:00 (10*60)

unsigned long lastTickMs = 0;

void avanzarReloj() {
    unsigned long now = millis();
    while ((unsigned long)(now - lastTickMs) >= 10) {
        lastTickMs += 10;
        fino++;
        if (fino >= 100) {
            fino = 0;
            segundero++;
            if (segundero >= 60) {
                segundero = 0;
                minutero++;
                if (minutero >= MINUTERO_MAX) minutero = 0;
            }
        }
    }
}

void setup() {
    // Serial es la UART del PIC (picLink_init(), 19200 baud) -- no una
    // consola de debug por USB. El PIC esta cableado al conector de
    // alimentacion de la placa (UART0, el mismo que el chip USB-serie de a
    // bordo), asi que no hay forma de usar Serial.print() sin inyectar
    // basura en el protocolo.
    display.init();

#if DEBUG_TIMING_ONLY
    digitalWrite(PIN_TFT_BL, HIGH);     // fuerza el backlight -- bypassea sleep/touch para este diagnostico
    Serial.begin(19200, SERIAL_8N1);    // misma UART/baud -- sin picLink_init(), no hace falta el resto
    display.drawTimingLine(0, 0);       // marca de arranque (2026-07-27) -- confirma que el ESP32 llego
                                         // hasta aca y el display funciona, independiente de si ya llego
                                         // algun 0xAA real del PIC (se pisa solo con la primera linea real)
#else
    picLink_init();

    // arranca todo en sentinel -- recien se llena cuando llegue el primer frame real
    for (uint8_t i = 0; i < NUM_NODES; i++) {
        readings[i].presionRaw16 = SENTINEL_UINT16;
        readings[i].tempC        = SENTINEL_INT8;
        readings[i].vbat         = SENTINEL_INT8;
        readings[i].fino         = SENTINEL_FINO;
        readings[i].segundero    = SENTINEL_SEG;
        readings[i].minutero     = SENTINEL_MINUTO;
    }
#endif
}

#if DEBUG_TIMING_ONLY
#define TIMING_MAX_ROWS  15   // (320-4)/ROW_H redondeado a la baja -- cuantos renglones entran en pantalla antes de volver a empezar
unsigned long lastAaMs   = 0;   // 0 = todavia no llego ningun 0xAA
uint8_t       timingRow  = 0;   // renglon actual, wrap circular al llegar a TIMING_MAX_ROWS
#elif DEBUG_RAW_BYTES
unsigned long lastRawDrawMs = 0;
uint32_t      rawByteCount  = 0;
#elif DEBUG_SHOW_RAW_HEX
unsigned long lastHexDumpMs = 0;
#endif

void loop() {
#if DEBUG_TIMING_ONLY
  while(1) {
    yield();
    while (Serial.available()) {   // sin framing/CRC -- solo busca el byte 0xAA
        uint8_t b = (uint8_t)Serial.read();
        if (b == 0xAA) {
            unsigned long now = millis();
            uint32_t delta = (lastAaMs == 0) ? 0 : (uint32_t)(now - lastAaMs);
            lastAaMs = now;

            // Correccion de OSCTUNE del pic-g (2026-07-27): la medicion precisa vive
            // aca (millis() del ESP32, cristal) -- el ESP32 decide y le dice al PIC
            // que hacer, en vez de mandarle el valor crudo para que el PIC calcule.
            // bit0=1 acelerar (el PIC fue lento, delta>=60000), bit0=0 decelerar (fue
            // rapido). El primer intercambio de la sesion (delta==0, sin referencia
            // todavia) manda "decelerar" de forma espuria -- aceptado a proposito,
            // se autocorrige solo en la siguiente ronda.
            // Frame: [0]=0xAA [1]=flagByte (bit0) [2]=checksum ~flagByte [3]=0x55
            uint8_t flagByte = (delta >= 60000) ? 0x01 : 0x00;
            uint8_t ackBuf[4];
            ackBuf[0] = 0xAA;
            ackBuf[1] = flagByte;
            ackBuf[2] = (uint8_t)(~flagByte);
            ackBuf[3] = 0x55;
            Serial.write(ackBuf, sizeof(ackBuf));

            // dibuja apenas se mando el ACK -- sin esperar a acumular nada
            display.drawTimingLine(timingRow, delta);
            timingRow++;
            if (timingRow >= TIMING_MAX_ROWS) timingRow = 0;   // vuelve arriba, limpia en el proximo renglon
        }
    }
  }
#else
    avanzarReloj();

#if DEBUG_RAW_BYTES
    while (Serial.available()) {   // sin pasar por picLink_poll() -- ni framing ni CRC
        Serial.read();
        rawByteCount++;
    }
    unsigned long now = millis();
    if (now - lastRawDrawMs >= 300) {
        lastRawDrawMs = now;
        display.drawHexDump(nullptr, 0, 0, rawByteCount, 0, 0, 0, 0);   // reusa el "bytes:" del volcado, sin datos ni contadores de PicLink
    }
#else
    PicLinkEvent picEv = picLink_poll(readings, &lastGatewayId, &lastGatewayAddr);   // sigue drenando el UART y actualizando los contadores, se use o no la tabla

#if DEBUG_SHOW_RAW_HEX
    unsigned long now = millis();
    if (now - lastHexDumpMs >= 300) {
        lastHexDumpMs = now;
        static uint8_t rawFrame[200];   // FRAME_SIZE real (186) con margen -- picLink_rawFrameSize() da el largo valido
        picLink_getLastRawFrame(rawFrame, sizeof(rawFrame));   // false si todavia no se completo ningun intento -- rawFrame queda en 0 (static)
        display.drawHexDump(rawFrame, picLink_rawFrameSize(), picLink_currentBufLen(),
                             picLink_bytesTotal(), picLink_framesOk(),
                             picLink_discardTimeout(), picLink_discardHeader(), picLink_discardCrc());
    }
#else
    // el nodo (PicLinkEvent::NODE) ya actualizo readings[addr-1] adentro de
    // picLink_poll() -- la UI recien se refresca al llegar el latido, que
    // trae el buffer completo de la ronda (mismo criterio que antes tenia
    // el frame agregado de 186 bytes).
    if (picEv == PicLinkEvent::HEARTBEAT) {
        display.setReadings(readings);
        picLink_sendAck(fino, segundero, minutero);

        // repone sentinels para la ronda que arranca -- un nodo ausente en
        // la proxima ronda no debe arrastrar el dato de esta (mismo criterio
        // que antes tenia gw2esp_resetRound() del lado PIC, ahora aca porque
        // el buffer por-ronda paso a vivir en el ESP32)
        for (uint8_t i = 0; i < NUM_NODES; i++) {
            readings[i].presionRaw16 = SENTINEL_UINT16;
            readings[i].tempC        = SENTINEL_INT8;
            readings[i].vbat         = SENTINEL_INT8;
            readings[i].fino         = SENTINEL_FINO;
            readings[i].segundero    = SENTINEL_SEG;
            readings[i].minutero     = SENTINEL_MINUTO;
        }
    }

    // O:framesOk  I:discardTimeout (se corto antes de completar el frame de 18 bytes)
    // dt:frameIntervalMs -- intervalo entre los ultimos 2 LATIDOS (1/ronda,
    // ~60s esperado), no entre cada frame de nodo individual.
    // E (discardHeader+discardCrc) sacado de la barra de estado -- nunca se
    // vio un caso en banco, todo el descarte era I. Sigue disponible via
    // picLink_discardHeader()/discardCrc() para el volcado hex (DEBUG_SHOW_RAW_HEX).
    display.setDebugCounters(picLink_framesOk(), picLink_discardTimeout(),
                              picLink_frameIntervalMs());

    display.setClock(simDay, simMonth, simYear, minutero / 60, minutero % 60, segundero);
    display.setQueueCount(0);   // cola SD pendiente -- todavia no implementada
    display.update();
#endif   // DEBUG_SHOW_RAW_HEX
#endif   // DEBUG_RAW_BYTES
#endif   // DEBUG_TIMING_ONLY
}
