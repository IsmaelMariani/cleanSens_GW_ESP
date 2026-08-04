#pragma once

#include <stdint.h>
#include <stddef.h>
#include "SharedTypes.h"

// =============================================================
// PicLink.h
//
// Protocolo picg <-> ESP32 (2026-07-31, reemplaza el frame agregado de 186
// bytes -- ver comentario grande en gw2esp.h del lado picg para el diseño
// completo). 2 tipos de frame, cada uno con su propio ACK inmediato:
//
//   NODO   (picg -> ESP32, hasta 15x por ronda, 1 por segundo en el
//           drenado): relay de 1 registro de UTA. 13 bytes, CRC16 completo
//           (lleva dato de medicion real). Sin gatewayId/gatewayAddr --
//           sacados 2026-07-31, no tenian uso real todavia (ni display ni
//           nube), se agregan el dia que haga falta un multi-gateway real.
//           Tampoco lleva fraccion/segundero/minuto del nodo (mismo dia) --
//           el reloj del nodo no aporta nada nuevo, ver ra01.h del lado
//           picg. ACK: 5 bytes, confirma addr.
//   RESYNC (picg -> ESP32, 1x por ronda, al final del drenado): trae
//           cuantos ticks propios paso el picg desde el ultimo resync
//           CONFIRMADO (o el sentinel GW2ESP_ELAPSED_SENTINEL si no es
//           confiable -- boot o se salteo mas de 1 ronda) + flag de reset
//           propio. 7 bytes, checksum XOR (frame de control, sin dato de
//           medicion). El ESP32 compara ese elapsed contra lo que el
//           registro segundos reales (millis()) desde su propio ultimo ACK
//           de resync, decide INC/DEC/NOTOCAR (con deadband) y lo devuelve
//           junto con su fino/segundero actual (correccion de fase). ACK:
//           7 bytes.
//
// Parsing con resincronizacion byte a byte (a diferencia del diseño viejo,
// que solo validaba el header/CRC recien al completar el buffer entero) --
// un byte de mas o de menos (ruido del CH340 compartiendo UART0) ya no deja
// el parser desalineado indefinidamente.
// =============================================================

// -- comandos -- deben coincidir con GW2ESP_CMD_* de gw2esp.h (picg) --
#define GW2ESP_CMD_NODE    0x01
#define GW2ESP_CMD_RESYNC  0x02

// -- ordenes -- deben coincidir con GW2ESP_ORDEN_* de gw2esp.h (picg) --
#define GW2ESP_ORDEN_DEC      0x00
#define GW2ESP_ORDEN_INC      0x01
#define GW2ESP_ORDEN_NOTOCAR  0x02

enum class PicLinkEvent : uint8_t {
    NONE,          // nada nuevo este poll
    NODE,          // se valido y ACKeo un frame de nodo -- readings[addr-1] ya actualizado
    ROUND_CLOSED   // se valido y ACKeo el resync -- readings[] ya tiene el blanqueo de
                   // UTAs ausentes aplicado (ver tocadoEstaRonda en el .cpp). Este es el
                   // punto correcto para disparar el redibujo completo de pantalla, no
                   // en cada NODE (mismo criterio ya usado en el proyecto: actualizar
                   // dato es barato, redibujar es caro).
};

void picLink_init();

// Llamar en cada vuelta de loop(), con fino/segundero YA actualizados
// (avanzarReloj() antes de esta llamada) -- si el poll valida un frame de
// resync, el ACK sale con esa hora en el mismo llamado, sin round-trip
// extra de latencia.
PicLinkEvent picLink_poll(NodeReading readings[NUM_NODES], uint8_t fino, uint8_t segundero);

// -- contadores de debugging (barra de estado, sin Monitor Serie en banco) --
uint32_t picLink_framesOk();
uint32_t picLink_discardTimeout();   // gap entre bytes o racha cortada a mitad de frame
uint32_t picLink_discardHeader();    // header/cmd/footer no matchea (frame corrido)
uint32_t picLink_discardCrc();       // CRC16 (nodo) o checksum XOR (resync) invalido

// offset firmado (ms) del ultimo resync: (tiempo real medido por el ESP32) -
// (tiempo que el picg cree que paso, segun su propio elapsed). Positivo =
// el picg corrio lento. INT32_MIN si el ultimo resync no fue confiable
// (sentinel) o todavia no hubo ninguno.
int32_t picLink_offsetMs();
