#include <Arduino.h>
#include <math.h>
#include <WiFi.h>
#include "gateway_pinout.h"
#include "SharedTypes.h"
#include "Display.h"
#include "PicLink.h"
#include "WifiConfig.h"
#include "DataPost.h"
#include "RealClock.h"

// =============================================================
// cleanSensGateway_v001.ino
// Etapa 3 (2026-07-31): UI real + protocolo de frames chicos por UTA
// (relay 1x1 durante el drenado + resync de cierre de ronda, ver
// PicLink.h) con correccion de OSCTUNE/fase del pic-g.
// Paquete 1 (WiFi/NTP) integrado -- conexion+config remota (WifiConfig.h),
// hora real (RealClock.h) y POST de datos 1x/ronda (DataPost.h).
// WIFICONFIG_STANDALONE_TEST abajo aisla WifiConfig para probarlo con el
// PIC fisicamente desconectado (Serial.print libre) -- dejar en 0 para
// operar con el PIC conectado.
// =============================================================

#define WIFICONFIG_STANDALONE_TEST  0

DisplayManager display;
NodeReading    readings[NUM_NODES];

// -- reloj local de 10ms (fino/segundero/minutero) -- mismo patron que
// synctime/minuto del lado PIC (ver synctime.c/minuto.h), SIN relacion con
// la hora real: esto es lo que le pasa a picLink_poll() para el protocolo
// de sync con el PICG (ver loop() mas abajo), nunca se toca por NTP.
// Fecha/hora real (display + fyhMedido del POST) sale de RealClock.h --
// separado a proposito, con fallback a esta fecha simulada mientras
// todavia no hay sync NTP (ver loop()).
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

#if WIFICONFIG_STANDALONE_TEST

// -- test aislado de WifiConfig, PIC fisicamente desconectado --
// Serial.print libre aca (115200, consola USB normal) porque no hay UART
// del PIC compitiendo por UART0 en este modo.
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("[WifiConfig test] arrancando...");

    WiFi.mode(WIFI_STA);
    Serial.println("Escaneando redes visibles...");
    int n = WiFi.scanNetworks();
    if (n == 0) {
        Serial.println("  (ninguna red visible)");
    } else {
        for (int i = 0; i < n; i++) {
            Serial.printf("  %2d) %-32s %ddBm\n", i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i));
        }
    }

    wifiConfig_init();

    Serial.print("semilla (fallback): ");
    Serial.println(wifiConfig_fallbackStr());
    Serial.print("activa en flash (antes de conectar): ");
    Serial.println(wifiConfig_activaStr());

    wifiConfig_bootstrap();

    Serial.print("activa en flash (despues del bootstrap): ");
    Serial.println(wifiConfig_activaStr());

    // ejercita la MISMA task/cola de DataPost que crashea en produccion --
    // con el PIC desconectado nunca llega ROUND_CLOSED, asi que sin esto
    // dataPost_encolar() nunca se llamaria en este modo. Datos falsos (todo
    // sentinel), no importa el contenido -- el objetivo es reproducir el
    // crash con Serial disponible para ver el panic real.
    dataPost_init();
}

void loop() {
    static unsigned long lastPrint = 0;
    if ((unsigned long)(millis() - lastPrint) > 5000) {
        lastPrint = millis();
        if (wifiConfig_conectado()) {
            Serial.printf("conectado, ip=%s\n", wifiConfig_ipStr().c_str());
        } else {
            // WiFi.status(): 0=IDLE 1=NO_SSID_AVAIL 2=SCAN_COMPLETED
            // 3=CONNECTED 4=CONNECT_FAILED 5=CONNECTION_LOST 6=DISCONNECTED
            Serial.printf("no conectado, WiFi.status()=%d\n", (int)WiFi.status());
        }
    }

    // TX de prueba cada 20s (dato falso, todo sentinel) -- fuerza el mismo
    // camino de codigo (dataPost_encolar -> task de Core 0 -> POST real a
    // GAS) que crashea en produccion, mas seguido que en produccion (2min)
    // para reproducir rapido con Serial disponible, pero con margen para leer.
    static unsigned long lastTx = 0;
    static uint32_t lastSeqSeen = 0;
    if ((unsigned long)(millis() - lastTx) > 20000) {
        lastTx = millis();
        NodeReading dummy[NUM_NODES];
        for (uint8_t i = 0; i < NUM_NODES; i++) {
            dummy[i].presionMmH2O = SENTINEL_INT16;
            dummy[i].tempC        = SENTINEL_INT8;
            dummy[i].vbat         = SENTINEL_INT8;
            dummy[i].idNodo       = SENTINEL_ID;
        }
        Serial.println(">> encolando TX de prueba...");
        dataPost_encolar(dummy, 0);
    }
    uint32_t seq = dataPost_resultSeq();
    if (seq != lastSeqSeen) {
        lastSeqSeen = seq;
        Serial.printf("<< resultado TX: wifiOk=%d txOk=%d (seq=%u)\n",
                       (int)dataPost_ultimoWifiOk(), (int)dataPost_ultimoOk(), (unsigned)seq);
    }
}

#else

void setup() {
    // Serial es la UART del PIC (picLink_init(), 19200 baud) -- no una
    // consola de debug por USB. El PIC esta cableado al conector de
    // alimentacion de la placa (UART0, el mismo que el chip USB-serie de a
    // bordo), asi que no hay forma de usar Serial.print() sin inyectar
    // basura en el protocolo.
    display.init();
    display.showBootMessage("Buscando WiFi...");

    picLink_init();
    wifiConfig_init();
    wifiConfig_bootstrap();
    realClock_init();   // configTime() -- el sync real llega asincronico, ver loop()
    dataPost_init();

    display.endBootMessage();

    // arranca todo en sentinel -- recien se llena cuando llegue el primer frame real
    for (uint8_t i = 0; i < NUM_NODES; i++) {
        readings[i].presionMmH2O = SENTINEL_INT16;
        readings[i].tempC        = SENTINEL_INT8;
        readings[i].vbat         = SENTINEL_INT8;
        readings[i].idNodo       = SENTINEL_ID;
    }
}

void loop() {
    avanzarReloj();
    wifiConfig_mantener();   // no bloqueante -- reintenta cada 30s si esta caido

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
    // TX a GAS cada 2 rondas (~2min, no cada ronda) -- dataPost_encolar()
    // nunca bloquea esta vuelta de loop(), el POST real corre en la task de
    // Core 0 (ver DataPost.h).
    if (picEv == PicLinkEvent::ROUND_CLOSED) {
        display.setReadings(readings);

        static uint8_t rondasParaTx = 0;
        if (++rondasParaTx >= 2) {
            rondasParaTx = 0;
            dataPost_encolar(readings, realClock_epochMs());   // epochMs=0 (fyhMedido:null) si RealClock todavia no sincronizo
        }
    }

    // resultado de la ultima TX (no WiFi.status() en vivo, ver DataPost.h)
    display.setInternetOk(dataPost_ultimoOk());

    // hora real (RealClock) una vez sincronizada -- antes del primer sync
    // NTP (o si se cae por mucho tiempo antes de arrancar) sigue mostrando
    // el reloj simulado local, nunca se congela la pantalla.
    if (realClock_valido()) {
        uint8_t rDay, rMonth, rYear, rHour, rMinute, rSecond;
        realClock_obtener(rDay, rMonth, rYear, rHour, rMinute, rSecond);
        display.setClock(rDay, rMonth, rYear, rHour, rMinute, rSecond);
    } else {
        display.setClock(simDay, simMonth, simYear, minutero / 60, minutero % 60, segundero);
    }

    display.setQueueCount(0);   // cola SD pendiente -- todavia no implementada
    display.update();
}

#endif  // WIFICONFIG_STANDALONE_TEST
