// 2026-09-30 19:23 - Nuevo: ciclo de radio WiFi (on solo en ventanas)
#include "WifiPower.h"
#include "WifiConfig.h"
#include "DataPost.h"
#include "RealClock.h"
#include "GatewayConfig.h"
#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// =============================================================
// WifiPower.cpp -- ver WifiPower.h para el diseño.
// =============================================================

#define WIFIPOWER_TASK_STACK_BYTES  4096
#define WIFIPOWER_TASK_CORE         0      // nunca Core 1 (loop()/picLink_poll/UART)

static volatile bool s_iniciado      = false;
static volatile bool s_enlaceListo   = false;   // ventana abierta y radio conectado
static volatile bool s_ventanaPedida = false;   // escrita por Core 1, consumida por la task

static void esperar(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// hasta WIFIPOWER_CONNECT_ATTEMPTS intentos de conexion con la credencial
// activa; entre intentos apaga el radio y espera WIFIPOWER_RETRY_DELAY_MS
static bool conectarVentana() {
    for (uint8_t i = 0; i < WIFIPOWER_CONNECT_ATTEMPTS; i++) {
        wifiConfig_iniciarConexion();
        uint32_t t0 = millis();
        while ((uint32_t)(millis() - t0) < WIFIPOWER_CONNECT_TIMEOUT_MS) {
            if (WiFi.status() == WL_CONNECTED) {
                DBG("[wp] conectado en %lu ms (intento %u)\n", (unsigned long)(millis() - t0), (unsigned)(i + 1));
                return true;
            }
            esperar(100);
        }
        DBG("[wp] no conecto (intento %u)\n", (unsigned)(i + 1));
        wifiConfig_apagar();
        if (i + 1 < WIFIPOWER_CONNECT_ATTEMPTS) esperar(WIFIPOWER_RETRY_DELAY_MS);
    }
    return false;
}

// ventana con el radio ya conectado. esBoot=true: la ventana de instalacion
// del boot (dura WIFIPOWER_BOOT_WINDOW_MS como minimo y mantiene la conexion
// con wifiConfig_mantener()); false: ventana normal de un bloque.
static void ventanaOnline(bool esBoot) {
    dataPost_nuevaVentana();
    realClock_resincronizar();
    s_enlaceListo = true;

    uint32_t tInicio = millis();   // inicio de la ventana (se reinicia si llega un pedido nuevo, salvo en boot)
    uint32_t tNtp    = millis();

    for (;;) {
        esperar(200);

        if (s_ventanaPedida) {
            s_ventanaPedida = false;
            dataPost_nuevaVentana();
            if (!esBoot) tInicio = millis();   // pedido nuevo con la ventana abierta: se extiende
        }

        if (esBoot) wifiConfig_mantener();     // reconecta si se cayo (no bloqueante)

        bool ntpOk    = realClock_sincronizoReciente() || ((uint32_t)(millis() - tNtp) >= WIFIPOWER_NTP_WAIT_MS);
        bool inactivo = dataPost_inactivo();
        uint32_t el   = (uint32_t)(millis() - tInicio);

        if (esBoot) {
            if (el >= WIFIPOWER_BOOT_WINDOW_MS && inactivo && ntpOk) break;
        } else {
            if (inactivo && ntpOk) break;
            if (el >= WIFIPOWER_ONLINE_MAX_MS) break;
        }
    }

    s_enlaceListo = false;   // DataPost no empieza envios nuevos

    // si quedo un POST en vuelo (tope de tiempo), esperarlo antes de apagar --
    // apagar el radio con un socket TLS en uso puede colgar/crashear lwIP
    uint32_t tEspera = millis();
    while (dataPost_enVuelo() && (uint32_t)(millis() - tEspera) < WIFIPOWER_INFLIGHT_WAIT_MS) {
        esperar(100);
    }
}

static void wifiPowerTask(void *param) {
    (void)param;

    // boot: wifiConfig_bootstrap() (bloqueante, en setup()) ya conecto -- o no;
    // en ese caso wifiConfig_mantener() sigue intentando durante la ventana
    ventanaOnline(true);
    wifiConfig_apagar();
    DBG("[wp] fin ventana de boot, radio apagado\n");

    for (;;) {
        if (!s_ventanaPedida) {
            esperar(200);
            continue;
        }
        s_ventanaPedida = false;

        if (conectarVentana()) {
            ventanaOnline(false);
        }
        wifiConfig_apagar();
        DBG("[wp] ventana cerrada, radio apagado\n");
    }
}

void wifiPower_init() {
    s_iniciado = true;
    xTaskCreatePinnedToCore(wifiPowerTask, "wifiPowerTask",
                             WIFIPOWER_TASK_STACK_BYTES, nullptr, 1, nullptr,
                             WIFIPOWER_TASK_CORE);
}

void wifiPower_solicitarVentana() {
    s_ventanaPedida = true;
}

bool wifiPower_enlaceListo() {
    if (!s_iniciado) return WiFi.status() == WL_CONNECTED;
    return s_enlaceListo;
}

bool wifiPower_iniciado() {
    return s_iniciado;
}
