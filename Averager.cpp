#include "Averager.h"

// =============================================================
// Averager.cpp -- ver Averager.h para el diseño.
// =============================================================

struct NodeAccum {
    int32_t  sumPresion;    // suma en centesimas de mmH2O -- 30 muestras de +-9999 caben holgado en int32_t
    uint16_t countPresion;  // cuantas lecturas validas se acumularon esta ventana
    int8_t   lastTemp;
    int8_t   lastVbat;
    uint8_t  idNodo;
};

static NodeAccum s_accum[NUM_NODES];

static void resetAccum() {
    for (uint8_t i = 0; i < NUM_NODES; i++) {
        s_accum[i].sumPresion   = 0;
        s_accum[i].countPresion = 0;
        s_accum[i].lastTemp     = SENTINEL_INT8;
        s_accum[i].lastVbat     = SENTINEL_INT8;
        s_accum[i].idNodo       = SENTINEL_ID;
    }
}

void averager_init() {
    resetAccum();
}

void averager_agregar(const NodeReading readings[NUM_NODES]) {
    for (uint8_t i = 0; i < NUM_NODES; i++) {
        if (readings[i].valid()) {
            s_accum[i].sumPresion += readings[i].presionMmH2O;
            s_accum[i].countPresion++;
            s_accum[i].lastTemp = readings[i].tempC;
            s_accum[i].lastVbat = readings[i].vbat;
            s_accum[i].idNodo   = readings[i].idNodo;
        }
    }
}

void averager_calcularYresetear(NodeReading out[NUM_NODES]) {
    for (uint8_t i = 0; i < NUM_NODES; i++) {
        if (s_accum[i].countPresion > 0) {
            out[i].presionMmH2O = (int16_t)(s_accum[i].sumPresion / s_accum[i].countPresion);
            out[i].tempC        = s_accum[i].lastTemp;
            out[i].vbat         = s_accum[i].lastVbat;
            out[i].idNodo       = s_accum[i].idNodo;
        } else {
            out[i].presionMmH2O = SENTINEL_INT16;
            out[i].tempC        = SENTINEL_INT8;
            out[i].vbat         = SENTINEL_INT8;
            out[i].idNodo       = SENTINEL_ID;
        }
    }
    resetAccum();
}
