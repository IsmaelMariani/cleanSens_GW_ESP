#include <Arduino.h>
#include <math.h>
#include "gateway_pinout.h"
#include "SharedTypes.h"
#include "Display.h"
#include "PicLink.h"

// =============================================================
// cleanSensGateway_v001.ino
// Etapa 3 (2026-07-31): UI real + protocolo de frames chicos por UTA
// (relay 1x1 durante el drenado + resync de cierre de ronda, ver
// PicLink.h) con correccion de OSCTUNE/fase del pic-g. WiFi/POST todavia
// pendiente.
// =============================================================

DisplayManager display;
NodeReading    readings[NUM_NODES];

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
    picLink_init();

    // arranca todo en sentinel -- recien se llena cuando llegue el primer frame real
    for (uint8_t i = 0; i < NUM_NODES; i++) {
        readings[i].presionMmH2O = SENTINEL_INT16;
        readings[i].tempC        = SENTINEL_INT8;
        readings[i].vbat         = SENTINEL_INT8;
    }
}

void loop() {
    avanzarReloj();

    // fino/segundero ya estan al dia (avanzarReloj() arriba) antes de este
    // llamado -- si el poll valida un frame de resync, el ACK sale con esa
    // hora en el mismo llamado, sin round-trip extra de latencia (ver
    // PicLink.cpp).
    PicLinkEvent picEv = picLink_poll(readings, fino, segundero);

    // NODE: PicLink.cpp ya actualizo readings[addr-1] y mando su ACK -- no
    // hace falta hacer nada aca, el redibujo se pospone a ROUND_CLOSED para
    // no titilar la pantalla hasta 15x por minuto (mismo criterio que ya
    // evito el titileo de 1x/seg en la version anterior).
    // ROUND_CLOSED: el resync ya se proceso (ACK mandado, OSCTUNE/fase del
    // pic-g corregidos del otro lado) y PicLink.cpp ya blanqueo las UTAs
    // ausentes esta ronda -- recien aca conviene redibujar la tabla completa.
    if (picEv == PicLinkEvent::ROUND_CLOSED) {
        display.setReadings(readings);
    }

    // O:framesOk  H:discardHeader (frame corrido)  K:discardCrc (bit corrupto)
    // off:offsetMs -- offset firmado contra el elapsed que reporta el propio
    // picg, deberia converger a ~0 con la correccion de OSCTUNE.
    display.setDebugCounters(picLink_framesOk(), picLink_discardHeader(),
                              picLink_discardCrc(), picLink_offsetMs());

    display.setClock(simDay, simMonth, simYear, minutero / 60, minutero % 60, segundero);
    display.setQueueCount(0);   // cola SD pendiente -- todavia no implementada
    display.update();
}
