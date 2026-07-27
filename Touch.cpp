#include <Arduino.h>
#include "Touch.h"

// =============================================================
// Touch.cpp
// =============================================================

void TouchManager::init() {
    _spi.begin(PIN_TOUCH_CLK, PIN_TOUCH_MISO, PIN_TOUCH_MOSI, PIN_TOUCH_CS);
    _ts.begin(_spi);
}

bool TouchManager::read(int16_t &x, int16_t &y) {
    // no usamos _ts.touched() -- trae Z_THRESHOLD=300 hardcodeado en la
    // libreria, sin API para bajarlo. Leemos la presion cruda nosotros.
    TS_Point p = _ts.getPoint();
    if (p.z < MIN_Z) return false;

    // el panel de touch esta rotado 90 grados respecto al LCD en esta
    // placa -- confirmado en banco (mover horizontal cambiaba de fila,
    // que depende del vertical). El canal Y crudo del chip es el que
    // sigue el horizontal de pantalla (invertido); el X crudo sigue
    // el vertical (directo).
    long mx = (long)(RAW_Y_MAX - p.y) * 240 / (RAW_Y_MAX - RAW_Y_MIN);
    long my = (long)(p.x - RAW_X_MIN) * 320 / (RAW_X_MAX - RAW_X_MIN);

    x = (int16_t)constrain(mx, 0, 239);
    y = (int16_t)constrain(my, 0, 319);

    // (print de debug de calibracion sacado -- Serial es ahora la UART del
    // PIC (ver PicLink.cpp), cualquier print ahi mete basura en medio del
    // protocolo binario. Si hace falta recalibrar, usar el volcado hex en
    // pantalla en su lugar, no Serial.)

    return true;
}
