#pragma once

#include <stdint.h>

// =============================================================
// SharedTypes.h
// Modelo de datos de los 15 nodos de presion diferencial
// =============================================================

#define NUM_NODES  15

// -- sentinels de "sin dato" (nodo ausente en la ronda) --
// convencion: minimo representable del tipo, fuera de cualquier
// rango fisico real de estos campos
#define SENTINEL_INT8    ((int8_t)0x80)   // -128
#define SENTINEL_INT16   ((int16_t)0x8000) // -32768, fuera de +-9999 (rango real
                                            // clampeado de presionMmH2O)
#define SENTINEL_ID      ((uint8_t)0xFF)   // 255, "sin nodo fisico asignado"

// -- lectura de un nodo, tal como llega relayada del picg (ver PicLink.cpp) --
// 2026-07-31: sin fino/segundero/minutero -- el nodo ya no manda su propio
// reloj en el uplink (ver nota grande en ra01.h del lado picg: se disciplina
// via el ACK, y el timestamp autoritativo del dato lo pone GAS al recibir,
// no el nodo). valid() paso a usar presionMmH2O como gate.
struct NodeReading {
    int16_t  presionMmH2O;  // centesimas de mmH2O, con signo (ej. 1234 = +12,34mm) --
                            // clampeado a +-9999 en el nodo (rango real +-101,6mm,
                            // ver abp2_convertPressure() del lado picg)
    int8_t   tempC;         // temperatura del sensor ABP2 (compensada), no del PIC
    int8_t   vbat;          // decimas de volt (ej. 23 = 2,3V) -- rango util ~18-33 para 2xAA (1,8-3,3V)
    uint8_t  idNodo;        // id de fabrica del nodo fisico que reporto esta UTA en esta ronda
                            // (trazabilidad UTA<->nodo, ver hoja UTA-NODO) -- SENTINEL_ID si ausente

    bool valid() const {
        return presionMmH2O != SENTINEL_INT16;
    }
};
