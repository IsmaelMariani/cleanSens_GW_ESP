// 2026-09-30 19:44 - Evento boot (motivo de reset + contador) a GAS
// 2026-09-30 19:23 - Config comun: DEBUG, FASTTEST, tiempos radio
#pragma once

// =============================================================
// GatewayConfig.h -- opciones de compilacion y tiempos comunes.
// =============================================================

// Debug por Serial (USB de programacion, UART0, 115200) -- APAGADO por defecto:
// con 0 no se inicializa Serial ni se manda nada (el equipo va sin USB). Poner
// en 1 solo para diagnosticar. El PIC va por Serial2, no se ve afectado.
#define GATEWAY_DEBUG  0

// Prueba rapida del ciclo completo (promedio -> cola -> radio on -> POST ->
// radio off) sin PIC ni NTP: comprime los 15 min a FASTTEST_BLOCK_S segundos y
// genera lecturas sinteticas. Los POST van a las hojas REALES con
// "fyhMedido":null (fila con fecha 1970-01-01 = dato de prueba, borrar a mano).
// DEJAR EN 0 PARA PRODUCCION.
#define GATEWAY_FASTTEST  0

// etiqueta de firmware que viaja en el evento de boot (version de esta compilacion)
#define GATEWAY_FW_TAG  "wroom_v001_2026-09-30"

#if GATEWAY_DEBUG
  #define DBG(...)  Serial.printf(__VA_ARGS__)
#else
  #define DBG(...)  do { } while (0)
#endif

// -- tiempos del ciclo de radio (WifiPower.cpp) --
#if GATEWAY_FASTTEST
  #define FASTTEST_BLOCK_S                60        // "15 min" comprimidos
  #define FASTTEST_ROUND_MS               4000      // una "ronda" sintetica cada 4 s
  #define WIFIPOWER_BOOT_WINDOW_MS        60000UL   // radio prendido tras el boot
  #define WIFIPOWER_RETRY_DELAY_MS        10000UL   // entre intentos de conexion
#else
  #define WIFIPOWER_BOOT_WINDOW_MS        600000UL  // 10 min prendido tras el boot (instalacion)
  #define WIFIPOWER_RETRY_DELAY_MS        60000UL   // entre intentos de conexion
#endif
#define WIFIPOWER_CONNECT_TIMEOUT_MS      20000UL   // por intento de conexion
#define WIFIPOWER_CONNECT_ATTEMPTS        2         // intentos por ventana
#define WIFIPOWER_ONLINE_MAX_MS           240000UL  // tope de una ventana con radio prendido
#define WIFIPOWER_NTP_WAIT_MS             6000UL    // espera maxima del sync NTP por ventana
#define WIFIPOWER_INFLIGHT_WAIT_MS        40000UL   // espera maxima de un POST en vuelo antes de apagar
