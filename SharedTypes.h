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
#define SENTINEL_UINT16  ((uint16_t)0xFFFF) // fuera de rango real del crudo (10-90% de
                                            // fondo de escala) -- para presionRaw16
#define SENTINEL_FINO    ((uint8_t)0xFF)  // fuera de rango valido 0-99
#define SENTINEL_SEG     ((uint8_t)0xFF)  // fuera de rango valido 0-59
#define SENTINEL_MINUTO  ((uint16_t)0xFFFF) // fuera de rango valido 0-1439

// -- lectura de un nodo, tal como llega en el frame agregado PIC->ESP32 --
struct NodeReading {
    uint16_t presionRaw16;  // TEMPORAL: crudo del ABP2 (raw24>>8), sin convertir a
                            // mmH2O -- diagnostico de sincronizacion, mismo formato
                            // que el wire, sin escalar (ver PicLink.cpp)
    int8_t   tempC;         // temperatura del sensor ABP2 (compensada), no del PIC
    int8_t   vbat;          // decimas de volt (ej. 23 = 2,3V) -- rango util ~18-33 para 2xAA (1,8-3,3V)
    uint8_t  fino;          // 0-99, posicion fina dentro del segundo (~10ms) -- hora real de la foto al sensor
    uint8_t  segundero;     // 0-59
    uint16_t minutero;      // 0-1439, minuto del dia -- hora real de la foto al sensor

    bool valid() const {
        return fino != SENTINEL_FINO && minutero != SENTINEL_MINUTO;
    }
};
