// 2026-09-30 19:23 - Nuevo: ciclo de radio WiFi (on solo en ventanas)
#pragma once

#include <stdbool.h>

// =============================================================
// WifiPower.h
//
// Ciclo de radio WiFi para bajar consumo: el radio vive APAGADO y solo se
// prende en "ventanas" -- al cerrar cada bloque de 15 min (:00/:15/:30/:45),
// para mandar lo que haya en la cola de DataPost y refrescar NTP -- y se apaga
// de nuevo apenas termina.
//
// Corre en su propia task de FreeRTOS pinneada a Core 0: es la UNICA que
// prende/apaga/conecta el WiFi despues del boot (Core 1 -- loop()/PICG -- nunca
// lo toca, asi el bloqueo de WiFi.mode()/begin() no puede demorar el UART del
// PIC). DataPost (otra task de Core 0) solo manda cuando wifiPower_enlaceListo()
// dice que hay ventana abierta, nunca toca el WiFi.
//
// Secuencia de una ventana: conectar a la credencial ACTIVA (hasta
// WIFIPOWER_CONNECT_ATTEMPTS intentos, el celu de instalacion NO se usa aca,
// solo en el bootstrap del boot) -> DataPost envia la cola -> NTP -> apagar.
// Si no conecta, la cola queda intacta y se reintenta en la ventana siguiente.
//
// Tras el boot el radio queda prendido WIFIPOWER_BOOT_WINDOW_MS (10 min) para
// que el instalador pueda verificar la comunicacion; recien ahi entra el ciclo.
// =============================================================

// llamar 1x en setup(), DESPUES de wifiConfig_bootstrap() y dataPost_init()
void wifiPower_init();

// no bloqueante, llamar desde loop() (Core 1) cada vez que se cierra un bloque
// y ya se encolo el promedio -- pide abrir una ventana de radio
void wifiPower_solicitarVentana();

// true mientras hay una ventana abierta y el radio conectado -- lo consulta
// DataPost (Core 0). Si wifiPower_init() nunca se llamo (modo de test
// aislado), devuelve WiFi.status()==WL_CONNECTED.
bool wifiPower_enlaceListo();

// true si wifiPower_init() ya creo la task
bool wifiPower_iniciado();
