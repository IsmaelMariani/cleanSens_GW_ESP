// 2026-10-05 09:40 - CPU fija a 80 MHz en setup()
// 2026-09-30 19:44 - Evento boot (motivo de reset + contador) a GAS
// 2026-09-30 19:23 - Ciclo de radio, bloques 15 min, FASTTEST
// 2026-09-30 18:36 - Sin display/touch; Serial solo con GATEWAY_DEBUG
// 2026-09-30 18:27 - Copia de cleanSensGateway_v001 para ESP32-WROOM
// 2026-09-30 17:03 - Serial.begin(115200) debug; PIC por Serial2
#include <Arduino.h>
#include <math.h>
#include <time.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_system.h>
#include "gateway_pinout.h"
#include "GatewayConfig.h"
#include "SharedTypes.h"
#include "PicLink.h"
#include "WifiConfig.h"
#include "WifiPower.h"
#include "DataPost.h"
#include "RealClock.h"
#include "Averager.h"

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

// GATEWAY_DEBUG / GATEWAY_FASTTEST / DBG() viven en GatewayConfig.h

NodeReading    readings[NUM_NODES];

// evento de arranque: contador de reinicios persistente (flash, namespace "sys")
// + motivo del reset (esp_reset_reason()) -- se encola para GAS (ver DataPost.h)
static void registrarBoot() {
    Preferences p;
    p.begin("sys", false);
    uint32_t n = p.getUInt("boots", 0) + 1;
    p.putUInt("boots", n);
    p.end();
    dataPost_encolarBoot((uint8_t)esp_reset_reason(), n);
    DBG("[gw] boot #%lu motivo=%d\n", (unsigned long)n, (int)esp_reset_reason());
}

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
    // CPU fija a 80 MHz todo el tiempo (minimo que admite WiFi): ahorra consumo
    // vs 240 MHz y evita cambios de clock en marcha (jitter contra el sync del picg)
    setCpuFrequencyMhz(80);

    // 2026-09-30: el PIC va por Serial2 (GPIO16/17, 19200 baud, ver
    // picLink_init() y gateway_pinout.h) -- Serial (UART0, USB de
    // programacion) queda libre para debug, solo si GATEWAY_DEBUG=1.
#if GATEWAY_DEBUG
    Serial.begin(115200);
#endif
    DBG("[gw] arranque, buscando WiFi...\n");

    picLink_init();
    wifiConfig_init();
    wifiConfig_bootstrap();
    realClock_init();   // configTime() -- el sync real llega asincronico, ver loop()
    dataPost_init();
    registrarBoot();    // evento "boot" a la cola (se manda en la ventana de arranque)
    averager_init();
    wifiPower_init();   // task de Core 0: radio prendido 10 min tras el boot, despues solo en ventanas

    DBG("[gw] init ok\n");

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
    // el WiFi lo maneja WifiPower.cpp (task de Core 0) -- loop() no lo toca, asi
    // prender/apagar/conectar el radio nunca demora picLink_poll()

    // fino/segundero ya estan al dia (avanzarReloj() arriba) antes de este
    // llamado -- si el poll valida un frame de resync, el ACK sale con esa
    // hora en el mismo llamado, sin round-trip extra de latencia (ver
    // PicLink.cpp).
    PicLinkEvent picEv = picLink_poll(readings, fino, segundero);

    // NODE: PicLink.cpp ya actualizo readings[addr-1] y mando su ACK -- no
    // hace falta hacer nada aca.
    // ROUND_CLOSED: el resync ya se proceso (ACK mandado, OSCTUNE/fase del
    // pic-g corregidos del otro lado) y PicLink.cpp ya blanqueo las UTAs
    // ausentes esta ronda -- recien aca conviene acumular la ronda completa.
    if (picEv == PicLinkEvent::ROUND_CLOSED) {
        averager_agregar(readings);      // acumula para el promedio del proximo POST (ver Averager.h)
#if GATEWAY_DEBUG
        for (uint8_t i = 0; i < NUM_NODES; i++) {
            if (readings[i].valid()) {
                DBG("[gw] UTA%02u id=%u p=%d t=%d v=%d\n", (unsigned)(i + 1), (unsigned)readings[i].idNodo,
                    (int)readings[i].presionMmH2O, (int)readings[i].tempC, (int)readings[i].vbat);
            }
        }
#endif
    }

#if GATEWAY_FASTTEST
    // lecturas sinteticas (sin PIC): una "ronda" cada FASTTEST_ROUND_MS. UTAs
    // 13-15 siempre ausentes, el resto con huecos periodicos -- ejercita el
    // promedio por cantidad real de muestras y las UTAs sin ninguna lectura.
    static unsigned long lastRondaMs = 0;
    static uint32_t      rondaN      = 0;
    if ((unsigned long)(millis() - lastRondaMs) >= FASTTEST_ROUND_MS) {
        lastRondaMs = millis();
        rondaN++;
        NodeReading sint[NUM_NODES];
        for (uint8_t i = 0; i < NUM_NODES; i++) {
            bool hueco = (i >= 12) || (((rondaN + i) % 7) == 0);
            if (hueco) {
                sint[i].presionMmH2O = SENTINEL_INT16;
                sint[i].tempC        = SENTINEL_INT8;
                sint[i].vbat         = SENTINEL_INT8;
                sint[i].idNodo       = SENTINEL_ID;
            } else {
                sint[i].presionMmH2O = (int16_t)(1000 + i * 10 + (rondaN % 5));
                sint[i].tempC        = 22;
                sint[i].vbat         = 28;
                sint[i].idNodo       = (uint8_t)(i + 1);
            }
        }
        averager_agregar(sint);
    }
#endif

    // cierre de bloque de AVERAGER_INTERVAL_MIN minutos: con la hora real los
    // bloques caen en :00/:15/:30/:45 (indice = epoch / duracion del bloque;
    // 15 min divide justo el offset GMT-3). Se dispara al CRUZAR al bloque
    // siguiente (no por "segundo == 0"), asi un loop demorado o un salto de
    // NTP no se lo pierde. El instante de cierre (limite del bloque) es el
    // fyhMedido del registro -- clave unica por bloque. Sin hora real no se
    // cierran bloques (las lecturas siguen acumulandose en el Averager).
    // Chequeo limitado a 1x cada 200ms -- loop() corre miles de veces por
    // segundo y leer el reloj tan seguido no aporta nada.
    static unsigned long lastClockCheckMs = 0;
    if ((unsigned long)(millis() - lastClockCheckMs) >= 200) {
        lastClockCheckMs = millis();

#if GATEWAY_FASTTEST
        bool     horaOk = true;   // la prueba rapida no depende de NTP
        uint32_t idx    = (uint32_t)(millis() / (FASTTEST_BLOCK_S * 1000UL));
#else
        bool     horaOk = realClock_valido();
        uint32_t idx    = horaOk ? (uint32_t)((uint64_t)time(nullptr) / (AVERAGER_INTERVAL_MIN * 60UL)) : 0;
#endif
        static bool     bloqueInit   = false;
        static uint32_t ultimoBloque = 0;

        if (horaOk) {
            if (!bloqueInit) {
                ultimoBloque = idx;   // primer dato de hora: el primer cierre es el proximo limite de bloque
                bloqueInit   = true;
            } else if (idx > ultimoBloque) {
                ultimoBloque = idx;

                NodeReading avgReadings[NUM_NODES];
                averager_calcularYresetear(avgReadings);   // promedio de presion + ultimo temp/vbat/idNodo valido del bloque
#if GATEWAY_FASTTEST
                int64_t epochCierreMs = 0;   // "fyhMedido":null -> 1970-01-01 en la hoja = dato de prueba
#else
                int64_t epochCierreMs = (int64_t)idx * AVERAGER_INTERVAL_MIN * 60 * 1000LL;
#endif
                dataPost_encolar(avgReadings, epochCierreMs);
                wifiPower_solicitarVentana();   // abre una ventana de radio para mandar la cola
                DBG("[gw] bloque %lu cerrado, cola=%u\n", (unsigned long)idx, (unsigned)dataPost_pendientes());
            }
        }
    }

#if GATEWAY_DEBUG
    // linea de estado cada 10 s: hora real, cola de DataPost y contadores
    static unsigned long lastStatusMs = 0;
    if ((unsigned long)(millis() - lastStatusMs) >= 10000) {
        lastStatusMs = millis();
        DBG("[gw] rtc=%s cola=%u entregados=%lu timeouts=%lu ambiguos=%lu perdidos=%lu enlace=%d\n",
            realClock_valido() ? "ok" : "sin sync", (unsigned)dataPost_pendientes(),
            (unsigned long)dataPost_entregados(), (unsigned long)dataPost_timeouts(),
            (unsigned long)dataPost_ambiguos(), (unsigned long)dataPost_perdidos(),
            (int)wifiPower_enlaceListo());
    }
#endif
}

#endif  // WIFICONFIG_STANDALONE_TEST
