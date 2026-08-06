#pragma once

#include <TFT_eSPI.h>
#include "SharedTypes.h"
#include "Touch.h"

// =============================================================
// Display.h  --  DisplayManager
// ILI9341  240x320  SPI  via TFT_eSPI  (vertical)
// Modos: TABLA (default) | GRAFICO (tubo tipo Dwyer)
// Sin touch 10s en GRAFICO -> vuelve a TABLA
// Sin touch 30s (total) -> apaga display
// =============================================================

typedef enum {
    MODE_TABLA,
    MODE_GRAFICO
} DisplayMode;

class DisplayManager {
public:
    void init();
    void update();   // llamar en cada vuelta de loop()

    // mensaje simple de arranque -- prende el backlight fuera de la maquina
    // de estados normal (touch/timeouts), pensado para el bootstrap
    // bloqueante de WiFi en setup() (hasta ~32s en el peor caso) para que la
    // pantalla no se vea "muerta". endBootMessage() apaga el backlight de
    // nuevo -- el diseño sigue siendo "apagado por defecto, on demand"
    // despues del arranque, esto no lo cambia.
    void showBootMessage(const char *msg);
    void endBootMessage();

    void setReadings(const NodeReading readings[NUM_NODES]);
    void setClock(uint8_t day, uint8_t month, uint8_t year,
                  uint8_t hour, uint8_t minute, uint8_t second);
    void setQueueCount(uint16_t count);

    // "I" verde/roja en la barra de estado -- resultado de la ultima TX
    // (dataPost_ultimoOk(), ver .ino), no WiFi.status() en vivo.
    void setInternetOk(bool ok);

    bool isOn() const { return _displayOn; }

    // maximo tiempo de redibujo observado (ms) -- cualquier draw call que
    // pase por update() (tabla completa, filas sueltas o grafico), medido
    // con millis(). Sirve para saber cuanto puede llegar a bloquear loop().
    uint32_t maxRedrawMs() const { return _maxRedrawMs; }

private:
    TFT_eSPI     _tft;
    TouchManager _touch;

    DisplayMode  _mode;
    bool         _displayOn;
    bool         _wasTouched;
    unsigned long _lastTouchMs;
    uint8_t      _selectedNode;

    NodeReading  _readings[NUM_NODES];
    uint8_t      _day, _month, _year, _hour, _minute, _second;
    uint16_t     _queueCount;
    bool         _internetOk;
    uint32_t     _maxRedrawMs;

    // dirty flags -- en MODE_TABLA hay dos niveles de redibujo (ver update()):
    // _needFullRedraw/_dataDirty disparan la pantalla completa (wake/cambio
    // de modo, o datos de nodos nuevos -- ~1x/min). La franja de arriba
    // (2026-08-05) se partio en 3 zonas independientes -- cada una redibuja
    // SOLO su propio rectangulo, no toda la franja, para no repintar (y
    // titilar) partes que no cambiaron: _dateDirty (fecha, cambia ~1x/dia),
    // _clockDirty (hora, 1x/seg), _internetDirty (resultado de la ultima
    // TX, ~1x/2min). _rowDirty se sigue usando en MODE_GRAFICO para saber
    // si el nodo que se esta viendo cambio.
    bool    _needFullRedraw;         // cambio de modo/nodo/wake -> redibujar todo
    bool    _dateDirty;
    bool    _clockDirty;
    bool    _internetDirty;
    bool    _dataDirty;              // cualquier lectura de nodo cambio -> redibuja la tabla completa
    bool    _rowDirty[NUM_NODES];    // por nodo -- usado en MODE_GRAFICO para el nodo seleccionado

    void    _turnOn();
    void    _turnOff();
    int8_t  _hitTestRow(int16_t y) const;

    void    _drawTablaFull();
    void    _drawStatusBar();
    void    _drawRows();        // todas -- unico camino de dibujo de filas ahora
    void    _drawRow(uint8_t i);
    void    _drawGrafico();
    void    _drawThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                            int16_t halfWidth, uint16_t color);

    // dibuja "<valor>" + circulito de grados + "C" -- evita depender de
    // que el simbolo grado este mapeado en la fuente. Devuelve la x
    // donde termino, por si sigue mas texto despues.
    int16_t _drawDegC(int8_t value, int16_t x, int16_t y, uint8_t font, uint16_t color);
};
