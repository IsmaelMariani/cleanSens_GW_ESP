// 2026-09-30 18:36 - Solo pines UART2 al PIC (sin display/touch/SD)
#pragma once

// =============================================================
// gateway_pinout.h
// Placa: ESP32-WROOM NodeMCU 38 pines, sin display, sin touch, sin SD.
// Unica conexion externa: alimentacion 3V3 + UART al PIC gateway.
// =============================================================

// -- UART PIC gateway (Serial2, UART2) --
// Cruzado con el PIC: RX del ESP <- TX del PIC, TX del ESP -> RX del PIC,
// masa comun. 19200 8N1. Serial (UART0, USB de programacion) queda libre
// para debug (ver GATEWAY_DEBUG en el .ino, apagado por defecto).
#define PIN_PIC_RX      16   // RX2 del ESP32 <- TX del PIC
#define PIN_PIC_TX      17   // TX2 del ESP32 -> RX del PIC
