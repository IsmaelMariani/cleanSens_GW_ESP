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
    // en banco). discardHeader/discardCrc nunca se vieron en banco (siempre
    // en 0) -- sacados de la barra de estado, quedan solo en el volcado hex
    // (drawHexDump) por si hace falta ese detalle mas adelante.
    // frameIntervalMs: tiempo entre los 2 ultimos frames OK -- para ver si
    // el ciclo del gateway (~60s esperados) se esta desincronizando.
    void setDebugCounters(uint32_t framesOk, uint32_t discardTimeout,
                          uint32_t frameIntervalMs);

    // volcado hex de un buffer crudo, pantalla completa -- debugging directo
    // del buffer de RX de la UART, bypassea tabla/grafico por completo.
    // currentBufLen: cuanto lleva acumulado el intento de frame EN CURSO
    // (0..186, independiente de si data/len son de un frame ya completo) --
    // para ver en tiempo real si esta entrando algo, sin esperar a que se
    // complete o se corte un frame entero.
    void drawHexDump(const uint8_t *data, size_t len, size_t currentBufLen,
                      uint32_t bytesTotal, uint32_t framesOk,
                      uint32_t discardTimeout, uint32_t discardHeader, uint32_t discardCrc);

    // TEMPORAL: debug crudo de timing (ver .ino, DEBUG_TIMING_ONLY) -- dibuja
    // UN renglon por llamada, apenas llega cada 0xAA (nada de esperar a
    // acumular un lote). "row" es el renglon de pantalla (wrap circular a
    // cargo del .ino): row==0 limpia la pantalla antes de dibujar, para
    // arrancar el ciclo siguiente.
    void drawTimingLine(uint8_t row, uint32_t deltaMs);

    bool isOn() const { return _displayOn; }

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
    uint32_t     _dbgFramesOk, _dbgDiscardTimeout, _dbgFrameIntervalMs;

    // dirty flags -- evitan redibujar todo cada 1s (causaba parpadeo)
    bool    _needFullRedraw;         // cambio de modo/nodo/wake -> redibujar todo
    bool    _statusDirty;            // fecha/hora/cola cambiaron -> solo la franja de arriba
    bool    _rowDirty[NUM_NODES];    // por nodo -- solo repinta la fila que realmente cambio

    void    _turnOn();
    void    _turnOff();
    int8_t  _hitTestRow(int16_t y) const;

    void    _drawTablaFull();
    void    _drawStatusBar();
    void    _drawRows();        // todas -- solo en el redibujo completo
    void    _drawDirtyRows();   // solo las marcadas en _rowDirty
    void    _drawRow(uint8_t i);
    void    _drawGrafico();
    void    _drawThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                            int16_t halfWidth, uint16_t color);

    // dibuja "<valor>" + circulito de grados + "C" -- evita depender de
    // que el simbolo grado este mapeado en la fuente. Devuelve la x
    // donde termino, por si sigue mas texto despues.
    int16_t _drawDegC(int8_t value, int16_t x, int16_t y, uint8_t font, uint16_t color);
};
