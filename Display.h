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

    void setReadings(const NodeReading readings[NUM_NODES]);
    void setClock(uint8_t day, uint8_t month, uint8_t year,
                  uint8_t hour, uint8_t minute, uint8_t second);
    void setQueueCount(uint16_t count);

    // contadores de debugging de PicLink -- se muestran en la barra de
    // estado; pensado para cuando no hay Monitor Serie (USB desconectado
    // en banco). discardHeader/discardCrc reemplazan al viejo discardTimeout
    // unico (2026-07-31) -- separan "frame corrido" (header/cmd/footer no
    // matchea) de "frame alineado pero con bit corrupto" (CRC/checksum
    // invalido), necesario para confirmar/descartar la hipotesis del CH340.
    // offsetMs: (tiempo real medido por el ESP32) - (tiempo que el picg cree
    // que paso, segun su propio resync) -- deberia converger a ~0 con la
    // correccion de OSCTUNE. INT32_MIN si el ultimo resync no fue confiable
    // o todavia no hubo ninguno.
    void setDebugCounters(uint32_t framesOk, uint32_t discardHeader,
                          uint32_t discardCrc, int32_t offsetMs);

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
    uint32_t     _dbgFramesOk, _dbgDiscardHeader, _dbgDiscardCrc;
    int32_t      _dbgOffsetMs;
    uint32_t     _maxRedrawMs;

    // dirty flags -- en MODE_TABLA hay dos niveles de redibujo (ver update()):
    // _statusDirty dispara SOLO la franja de arriba (1x/seg tipico, el
    // segundero tickea seguido via setClock()); _needFullRedraw/_dataDirty
    // disparan la pantalla completa (wake/cambio de modo, o datos de nodos
    // nuevos -- ~1x/min, cuando llega el frame agregado). _rowDirty se sigue
    // usando en MODE_GRAFICO para saber si el nodo que se esta viendo cambio.
    bool    _needFullRedraw;         // cambio de modo/nodo/wake -> redibujar todo
    bool    _statusDirty;            // fecha/hora/cola cambiaron -> redibuja solo la franja de arriba
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
