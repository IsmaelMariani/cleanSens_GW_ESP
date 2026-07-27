#pragma once

#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <stdint.h>
#include "gateway_pinout.h"

// =============================================================
// Touch.h  --  TouchManager
// XPT2046 resistivo, bus SPI de software propio (no comparte
// pines con el display) -- ver gateway_pinout.h
//
// Requiere la libreria "XPT2046_Touchscreen" (Paul Stoffregen)
// instalada via Library Manager -- el bit-bang casero anterior
// tenia los command bytes X/Y invertidos.
// =============================================================

class TouchManager {
public:
    TouchManager() : _ts(PIN_TOUCH_CS, PIN_TOUCH_IRQ) {}

    void init();

    // true si hay un toque valido -- x,y ya mapeados a pixeles
    // de pantalla (240x320, rotacion vertical == Display.cpp)
    bool read(int16_t &x, int16_t &y);

private:
    // HSPI explicito -- TFT_eSPI ya toma VSPI para el display en esta
    // placa; si el touch cayera en el mismo host por default, las dos
    // periferias pelean por el mismo SPI de hardware y lo leido sale
    // sin ninguna relacion con la posicion real tocada.
    SPIClass            _spi{HSPI};
    XPT2046_Touchscreen _ts;

    // calibracion -- rangos tipicos publicados para esta placa
    // (RandomNerdTutorials, CYD), A CONFIRMAR con los prints de
    // debug en read() contra tu unidad real
    static const uint16_t RAW_X_MIN = 200;
    static const uint16_t RAW_X_MAX = 3700;
    static const uint16_t RAW_Y_MIN = 240;
    static const uint16_t RAW_Y_MAX = 3800;

    // umbral de presion propio -- el touched() de la libreria trae
    // Z_THRESHOLD=300 hardcodeado sin forma de bajarlo, por eso leemos
    // p.z crudo con getPoint() y comparamos aca. Si hace falta mas
    // sensibilidad todavia, bajar mas; si empieza a detectar toques
    // fantasma (ruido), subir -- es un trade-off real del touch resistivo.
    static const uint16_t MIN_Z = 100;
};
