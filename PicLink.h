#pragma once

#include <stdint.h>
#include <stddef.h>
#include "SharedTypes.h"

// =============================================================
// PicLink.h
// Recepcion UART del frame que manda el PIC gateway (formato definido en
// gw2esp.h del firmware del PIC -- 18 bytes: header + gatewayId +
// gatewayAddr + 1 registro de nodo (12 bytes) + crc16 + footer). El
// gateway manda un frame por CADA nodo que reporta OK (addr 1..15,
// directo, sin buffer ni ACK -- ver nota en gw2esp.h del PIC) mas 1
// "latido" por ronda (addr=0, en el segundo 50) que es el UNICO que
// espera el ACK de vuelta (8 bytes: header+fraccion+segundero+minuto+
// crc16+footer, formato esp2gw_ack_t de gw2esp.c) para resincronizar el
// synctime/minuto del PIC.
// =============================================================

enum class PicLinkEvent : uint8_t { NONE, NODE, HEARTBEAT };

void picLink_init();

// Llamar en cada vuelta de loop(). Si el resultado es NODE, ya actualizo
// readings[addr-1] (el nodo que llego). Si es HEARTBEAT, readings[] NO se
// toca aca -- es responsabilidad del llamador pasarlo a la UI y despues
// resetearlo a sentinels para la ronda que arranca (para que un nodo
// ausente en la ronda nueva no arrastre el dato de la anterior). gatewayId/
// gatewayAddr se actualizan en cualquiera de los dos casos. Un frame
// invalido (CRC no matchea, etc.) se descarta en silencio (NONE).
PicLinkEvent picLink_poll(NodeReading readings[NUM_NODES], uint8_t *gatewayId, uint8_t *gatewayAddr);

// Arma y transmite el ACK ESP32->gateway (8 bytes) con la hora "real" del
// ESP32 (fraccion 0..99 dentro del segundo, segundero 0..59, minuto 0..1439
// del dia) -- llamar solo cuando picLink_poll() devuelve HEARTBEAT, para
// que el PIC pueda resincronizarse (gw2esp_waitAck() del lado PIC).
void picLink_sendAck(uint8_t fraccion, uint8_t segundero, uint16_t minuto);

// contadores acumulados de debugging -- pensados para mostrar en el display
// cuando no hay Monitor Serie disponible (USB desconectado en banco).
uint32_t picLink_bytesTotal();       // total de bytes recibidos (si queda en 0,
                                      // es cableado/baudrate, no framing/CRC)
uint32_t picLink_framesOk();         // frames validados OK (nodos + latidos)
uint32_t picLink_discardTimeout();   // se corto la racha antes de completar el frame
uint32_t picLink_discardHeader();    // se completo el frame pero header/footer no matchea
uint32_t picLink_discardCrc();       // header/footer ok pero CRC no matchea

// tiempo (ms) entre los dos ultimos LATIDOS validados OK -- 0 hasta que haya
// al menos 2 (~60s esperado, 1 latido por ronda). No se mide entre cada
// frame de nodo individual (esos son mas frecuentes, ~1/seg) -- se sigue
// midiendo entre rondas para el mismo diagnostico de desincronizacion.
uint32_t picLink_frameIntervalMs();

// tamaño del frame crudo (18 bytes) -- constante, pero expuesta para no
// duplicar el numero mas alla de este header
size_t picLink_rawFrameSize();

// numero de secuencia: se incrementa cada vez que se completa un intento
// de frame, se valide bien o no. Sirve para detectar "hay un dump
// nuevo para mostrar" sin necesitar comparar el contenido.
uint32_t picLink_captureSeq();

// copia el ultimo intento de frame completo a out, sea cual sea el
// resultado de su validacion. false si todavia no se completo ninguno.
// Pensado para debugging visual (volcado hex en el display).
bool picLink_getLastRawFrame(uint8_t *out, size_t outSize);

// cuantos bytes lleva acumulados el intento de frame actual --
// para ver en tiempo real si esta entrando algo, sin esperar a que se
// complete (o se corte) un frame entero.
size_t picLink_currentBufLen();
