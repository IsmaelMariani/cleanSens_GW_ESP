#include "Display.h"
#include "gateway_pinout.h"
#include <math.h>
#include <string.h>

// =============================================================
// Display.cpp
// =============================================================

// TEMPORAL: sin uso mientras se muestra el crudo en hex (ver nota en
// SharedTypes.h/ra01.h) -- queda para cuando se vuelva a convertir a mmH2O.
// Formatea siempre con signo explicito ("+12,34" / "-5,00"), nunca solo "-"
// implicito del %d para los negativos y nada para los positivos.
static void formatSignedHundredths(int16_t hundredths, char *out, size_t outSize, const char *suffix) __attribute__((unused));
static void formatSignedHundredths(int16_t hundredths, char *out, size_t outSize, const char *suffix) {
    char    sign  = (hundredths < 0) ? '-' : '+';
    int16_t whole = abs(hundredths);
    snprintf(out, outSize, "%c%d,%02d%s", sign, whole / 100, whole % 100, suffix);
}

// -- layout modo Tabla --
#define STATUS_H      20
#define ROW_H         20   // (320-STATUS_H)/NUM_NODES = 300/15 = 20 exacto

// columnas de la fila -- presion tiene la columna mas ancha (queda a la
// izquierda del divisor, centrada entre el ID y el divisor), temperatura
// y bateria corridas a la derecha
#define COL_DIVIDER_X   148
#define COL_TEMP_X      156
#define COL_VBAT_X      200
#define COL_MARGIN       6   // separacion minima del texto de presion respecto al ID y al divisor

// -- layout modo Grafico (tubo inclinado tipo Dwyer) --
#define TUBE_X0       40
#define TUBE_Y0       290
#define TUBE_X1       210
#define TUBE_Y1       50
#define TUBE_HALFW    8
#define SENSOR_RANGE_MM  102   // +-4"H2O ~ 101.6mm, redondeado

// -- timeouts --
#define TIMEOUT_GRAFICO_MS   10000
#define TIMEOUT_OFF_MS       30000

// =============================================================

void DisplayManager::init() {
    _tft.init();
    _tft.setRotation(0);   // vertical, 240x320
    _tft.fillScreen(TFT_BLACK);
    _touch.init();

    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, LOW);

    _displayOn      = false;
    _mode           = MODE_TABLA;
    _wasTouched     = false;
    _lastTouchMs    = 0;
    _selectedNode   = 0;
    _queueCount     = 0;
    _dbgFramesOk = _dbgDiscardTimeout = _dbgFrameIntervalMs = 0;
    _day = _month = _year = _hour = _minute = _second = 0;
    _needFullRedraw = false;
    _statusDirty    = false;
    memset(_rowDirty, 0, sizeof(_rowDirty));

    // arranca en sentinel ("sin dato"), no en ceros crudos -- 0 es un valor
    // valido en presion/temp/vbat, memset(0) haria que valid() diera true
    // con datos falsos hasta que llegue el primer frame real
    for (uint8_t i = 0; i < NUM_NODES; i++) {
        _readings[i].presionRaw16 = SENTINEL_UINT16;
        _readings[i].tempC        = SENTINEL_INT8;
        _readings[i].vbat         = SENTINEL_INT8;
        _readings[i].fino         = SENTINEL_FINO;
        _readings[i].segundero    = SENTINEL_SEG;
        _readings[i].minutero     = SENTINEL_MINUTO;
    }
}

void DisplayManager::setReadings(const NodeReading readings[NUM_NODES]) {
    // dirty por nodo -- si cambia un solo nodo, solo esa fila se repinta,
    // no las 15 (evita el parpadeo de la tabla completa)
    for (uint8_t i = 0; i < NUM_NODES; i++) {
        if (memcmp(&_readings[i], &readings[i], sizeof(NodeReading)) != 0) {
            _readings[i] = readings[i];
            _rowDirty[i] = true;
        }
    }
}

void DisplayManager::setClock(uint8_t day, uint8_t month, uint8_t year,
                               uint8_t hour, uint8_t minute, uint8_t second) {
    if (_day != day || _month != month || _year != year ||
        _hour != hour || _minute != minute || _second != second) {
        _day = day; _month = month; _year = year;
        _hour = hour; _minute = minute; _second = second;
        _statusDirty = true;
    }
}

void DisplayManager::setQueueCount(uint16_t count) {
    if (_queueCount != count) {
        _queueCount = count;
        _statusDirty = true;
    }
}

void DisplayManager::setDebugCounters(uint32_t framesOk, uint32_t discardTimeout,
                                       uint32_t frameIntervalMs) {
    if (_dbgFramesOk != framesOk || _dbgDiscardTimeout != discardTimeout ||
        _dbgFrameIntervalMs != frameIntervalMs) {
        _dbgFramesOk        = framesOk;
        _dbgDiscardTimeout  = discardTimeout;
        _dbgFrameIntervalMs = frameIntervalMs;
        _statusDirty = true;
    }
}

// -------------------------------------------------------------
// update() -- maquina de estados de touch/timeouts, llamar cada loop()
// -------------------------------------------------------------
void DisplayManager::update() {
    int16_t tx, ty;
    bool touched = _touch.read(tx, ty);
    bool tapEdge = touched && !_wasTouched;
    _wasTouched  = touched;

    unsigned long now = millis();

    if (tapEdge) {
        if (!_displayOn) {
            // el primer toque solo despierta -- no se interpreta como
            // seleccion de fila, para evitar abrir un nodo por accidente
            _turnOn();
        } else if (_mode == MODE_TABLA) {
            int8_t row = _hitTestRow(ty);
            if (row >= 0) {
                _selectedNode  = row;
                _mode          = MODE_GRAFICO;
                _needFullRedraw = true;
            }
        } else {   // MODE_GRAFICO -> mitad superior retrocede, inferior avanza (con wraparound)
            if (ty < 160) {
                _selectedNode = (_selectedNode == 0) ? (NUM_NODES - 1) : (_selectedNode - 1);
            } else {
                _selectedNode = (_selectedNode == NUM_NODES - 1) ? 0 : (_selectedNode + 1);
            }
            _needFullRedraw = true;
        }
        _lastTouchMs = now;
    }

    if (!_displayOn) return;

    unsigned long idle = now - _lastTouchMs;

    if (_mode == MODE_GRAFICO && idle >= TIMEOUT_GRAFICO_MS) {
        _mode           = MODE_TABLA;
        _needFullRedraw = true;
    }
    if (idle >= TIMEOUT_OFF_MS) {
        _turnOff();
        return;
    }

    // redibujo dirigido por cambios reales, no por un timer parejo --
    // evita el parpadeo de limpiar toda la pantalla o toda la tabla
    if (_mode == MODE_TABLA) {
        if (_needFullRedraw) {
            _drawTablaFull();
            _needFullRedraw = false;
            _statusDirty = false;
            memset(_rowDirty, 0, sizeof(_rowDirty));
        } else {
            if (_statusDirty) { _drawStatusBar(); _statusDirty = false; }
            _drawDirtyRows();   // solo repinta las filas que realmente cambiaron
        }
    } else {   // MODE_GRAFICO
        bool selectedDirty = _rowDirty[_selectedNode];
        if (_needFullRedraw || selectedDirty) {
            _drawGrafico();
            _needFullRedraw = false;
        }
        memset(_rowDirty, 0, sizeof(_rowDirty));   // se consumen igual, aunque no esten a la vista
    }
}

void DisplayManager::_turnOn() {
    _displayOn      = true;
    _mode           = MODE_TABLA;
    _needFullRedraw = true;
    digitalWrite(PIN_TFT_BL, HIGH);
}

void DisplayManager::_turnOff() {
    _displayOn = false;
    digitalWrite(PIN_TFT_BL, LOW);
    _tft.fillScreen(TFT_BLACK);
}

int8_t DisplayManager::_hitTestRow(int16_t y) const {
    if (y < STATUS_H) return -1;
    int8_t row = (y - STATUS_H) / ROW_H;
    return (row >= 0 && row < NUM_NODES) ? row : (int8_t)-1;
}

// -------------------------------------------------------------
// _drawTablaFull -- redibujo completo (solo al entrar al modo/wake)
// -------------------------------------------------------------
void DisplayManager::_drawTablaFull() {
    _tft.fillScreen(TFT_BLACK);
    _drawStatusBar();
    _drawRows();
}

// -------------------------------------------------------------
// _drawStatusBar -- solo la franja de arriba (fecha/hora/cola), 1x/seg
// -------------------------------------------------------------
void DisplayManager::_drawStatusBar() {
    _tft.fillRect(0, 0, 240, STATUS_H - 1, TFT_BLACK);

    // fecha temporalmente fuera (fija/ficticia, no aporta hoy) para hacer
    // lugar a los contadores O/I y al intervalo entre frames -- mismo
    // criterio ya usado el 21/7 con los contadores de debug anteriores
    // (R/T/H/K). E (discardHeader+discardCrc) sacado: nunca se vio un solo
    // caso en banco, todo el descarte observado es I (discardTimeout).
    char line[56];
    if (_dbgFrameIntervalMs == 0) {
        snprintf(line, sizeof(line), "%02d:%02d:%02d  Cola:%u  O:%lu I:%lu  dt:--",
                 _hour, _minute, _second, _queueCount,
                 (unsigned long)_dbgFramesOk,
                 (unsigned long)_dbgDiscardTimeout);
    } else {
        snprintf(line, sizeof(line), "%02d:%02d:%02d  Cola:%u  O:%lu I:%lu  dt:%lums",
                 _hour, _minute, _second, _queueCount,
                 (unsigned long)_dbgFramesOk,
                 (unsigned long)_dbgDiscardTimeout,
                 (unsigned long)_dbgFrameIntervalMs);
    }
    _tft.setTextColor(TFT_WHITE, TFT_BLACK);
    _tft.drawString(line, 4, 3, 2);
    _tft.drawFastHLine(0, STATUS_H - 1, 240, _tft.color565(60, 60, 60));
}

// -------------------------------------------------------------
// _drawRow -- una sola fila (id + presion/temp/vbat o ---)
// -------------------------------------------------------------
void DisplayManager::_drawRow(uint8_t i) {
    int16_t y = STATUS_H + i * ROW_H;
    _tft.fillRect(0, y, 240, ROW_H, TFT_BLACK);

    const NodeReading &r = _readings[i];

    char idbuf[8];
    snprintf(idbuf, sizeof(idbuf), "UTA%02d", i + 1);
    _tft.setTextColor(TFT_WHITE, TFT_BLACK);
    _tft.drawString(idbuf, 4, y + 2, 2);

    // presion centrada en el espacio libre entre el ID y el divisor vertical
    int16_t presionLeft  = 4 + _tft.textWidth(idbuf, 2) + COL_MARGIN;
    int16_t presionRight = COL_DIVIDER_X - COL_MARGIN;

    if (!r.valid()) {
        // mismo layout de columnas que el caso valido (mas abajo), pero con
        // "--" en vez de numero -- asi las unidades (mmH2O/C/V) no desaparecen.
        // "+--" (no solo "--") para que el ancho quede igual que un valor con
        // signo real y no salte al aparecer el primer dato.
        uint16_t dimColor = _tft.color565(120, 120, 120);
        _tft.setTextColor(dimColor, TFT_BLACK);

        const char *presionStr = "----";   // TEMPORAL: mismo ancho que el hex de 4 digitos
        int16_t presionW = _tft.textWidth(presionStr, 2);
        int16_t presionX = presionLeft + (presionRight - presionLeft - presionW) / 2;
        _tft.drawString(presionStr, presionX, y + 2, 2);

        _tft.drawString("--", COL_TEMP_X, y + 2, 2);
        int16_t numW = _tft.textWidth("--", 2);
        int16_t degR = 2;
        int16_t cx   = COL_TEMP_X + numW + degR + 1;
        int16_t cy   = y + 2 + degR;
        _tft.drawCircle(cx, cy, degR, dimColor);
        _tft.drawString("C", cx + degR + 2, y + 2, 2);

        _tft.drawString("--,-V", COL_VBAT_X, y + 2, 2);
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "%04X", r.presionRaw16);   // TEMPORAL: crudo en hex, ver SharedTypes.h
        _tft.setTextColor(TFT_WHITE, TFT_BLACK);
        int16_t presionW = _tft.textWidth(buf, 2);
        int16_t presionX = presionLeft + (presionRight - presionLeft - presionW) / 2;
        _tft.drawString(buf, presionX, y + 2, 2);

        _drawDegC(r.tempC, COL_TEMP_X, y + 2, 2, _tft.color565(180, 180, 180));

        snprintf(buf, sizeof(buf), "%d,%dV", r.vbat / 10, abs(r.vbat % 10));
        _tft.setTextColor(_tft.color565(180, 180, 180), TFT_BLACK);
        _tft.drawString(buf, COL_VBAT_X, y + 2, 2);
    }

    // divisor vertical entre presion y temp/vbat -- mismo color/estilo que
    // los divisores horizontales entre filas
    _tft.drawFastVLine(COL_DIVIDER_X, y, ROW_H, _tft.color565(30, 30, 30));

    if (i > 0) _tft.drawFastHLine(0, y, 240, _tft.color565(30, 30, 30));
}

// -------------------------------------------------------------
// _drawRows -- las 15 filas, solo se usa en el redibujo completo
// -------------------------------------------------------------
void DisplayManager::_drawRows() {
    for (uint8_t i = 0; i < NUM_NODES; i++) _drawRow(i);
}

// -------------------------------------------------------------
// _drawDirtyRows -- solo repinta las filas marcadas, y limpia el flag
// -------------------------------------------------------------
void DisplayManager::_drawDirtyRows() {
    for (uint8_t i = 0; i < NUM_NODES; i++) {
        if (_rowDirty[i]) {
            _drawRow(i);
            _rowDirty[i] = false;
        }
    }
}

// -------------------------------------------------------------
// drawTimingLine -- TEMPORAL, ver .ino DEBUG_TIMING_ONLY. Bypassea
// tabla/grafico/touch por completo, igual que drawHexDump. Dibuja un solo
// renglon apenas llega cada 0xAA -- row==0 limpia la pantalla primero
// (arranque de un ciclo nuevo tras el wrap circular que maneja el .ino).
// -------------------------------------------------------------
void DisplayManager::drawTimingLine(uint8_t row, uint32_t deltaMs) {
    if (row == 0) _tft.fillScreen(TFT_BLACK);
    _tft.setTextColor(TFT_WHITE, TFT_BLACK);
    char line[24];
    snprintf(line, sizeof(line), "%2d: %lu ms", row + 1, (unsigned long)deltaMs);
    _tft.drawString(line, 4, row * ROW_H + 4, 2);
}

// -------------------------------------------------------------
// drawHexDump -- volcado hex de un buffer crudo, pantalla completa.
// Debugging directo: bypassea tabla/grafico/touch por completo, no toca
// ningun dirty-flag -- se llama directo desde el .ino cuando PicLink
// captura un frame nuevo (se valide bien o no).
// -------------------------------------------------------------
void DisplayManager::drawHexDump(const uint8_t *data, size_t len, size_t currentBufLen,
                                  uint32_t bytesTotal, uint32_t framesOk,
                                  uint32_t discardTimeout, uint32_t discardHeader,
                                  uint32_t discardCrc) {
    if (!_displayOn) _turnOn();

    _tft.fillScreen(TFT_BLACK);
    _tft.setTextColor(TFT_WHITE, TFT_BLACK);

    char line[56];
    snprintf(line, sizeof(line), "bytes:%lu ok:%lu buf:%u/186",
             (unsigned long)bytesTotal, (unsigned long)framesOk, (unsigned)currentBufLen);
    _tft.drawString(line, 4, 2, 1);
    snprintf(line, sizeof(line), "T:%lu H:%lu K:%lu len:%u",
             (unsigned long)discardTimeout, (unsigned long)discardHeader,
             (unsigned long)discardCrc, (unsigned)len);
    _tft.drawString(line, 4, 14, 1);

    const uint8_t BYTES_PER_ROW = 12;
    int16_t y = 28;
    for (size_t i = 0; i < len; i += BYTES_PER_ROW) {
        char hexline[56];
        int  pos = snprintf(hexline, sizeof(hexline), "%03u:", (unsigned)i);
        for (size_t j = i; j < i + BYTES_PER_ROW && j < len; j++) {
            pos += snprintf(hexline + pos, sizeof(hexline) - (size_t)pos, " %02X", data[j]);
        }
        _tft.drawString(hexline, 2, y, 1);
        y += 11;
        if (y > 316) break;   // no seguir escribiendo fuera de pantalla
    }
}

// -------------------------------------------------------------
// _drawGrafico -- tubo inclinado tipo Dwyer, un nodo a pantalla completa
// (redibuja todo -- solo se llama al cambiar de nodo/modo o cuando
// cambian los datos, nunca por un timer parejo)
// -------------------------------------------------------------
void DisplayManager::_drawGrafico() {
    _tft.fillScreen(TFT_BLACK);

    char title[12];
    snprintf(title, sizeof(title), "Nodo %02d", _selectedNode + 1);
    _tft.setTextColor(TFT_WHITE, TFT_BLACK);
    _tft.drawCentreString(title, 120, 4, 4);

    const NodeReading &r = _readings[_selectedNode];

    if (!r.valid()) {
        _tft.setTextColor(_tft.color565(120, 120, 120), TFT_BLACK);
        _tft.drawCentreString("--- sin dato ---", 120, 150, 4);
        return;
    }

    // linea "T:<n>C  <n,n>V" centrada -- compuesta a mano (no drawCentreString)
    // porque hay que intercalar el circulito de grados en el medio
    {
        const uint8_t  FONT = 2;
        const uint16_t MCLR = _tft.color565(160, 160, 160);

        char tNum[6], vStr[10];
        snprintf(tNum, sizeof(tNum), "%d", r.tempC);
        snprintf(vStr, sizeof(vStr), "  %d,%dV", r.vbat / 10, abs(r.vbat % 10));

        int16_t wPrefix = _tft.textWidth("T:", FONT);
        int16_t wNum    = _tft.textWidth(tNum, FONT);
        const int16_t degR = 2;
        int16_t wDeg    = degR * 2 + 3;
        int16_t wC      = _tft.textWidth("C", FONT);
        int16_t wV      = _tft.textWidth(vStr, FONT);

        int16_t x = 120 - (wPrefix + wNum + wDeg + wC + wV) / 2;
        const int16_t y = 40;

        _tft.setTextColor(MCLR, TFT_BLACK);
        _tft.drawString("T:", x, y, FONT);   x += wPrefix;
        _tft.drawString(tNum, x, y, FONT);   x += wNum;
        _tft.drawCircle(x + degR + 1, y + degR, degR, MCLR);
        x += wDeg;
        _tft.drawString("C", x, y, FONT);    x += wC;
        _tft.drawString(vStr, x, y, FONT);
    }

    // TEMPORAL: r.presionRaw16 es el crudo del sensor (sin convertir, sin signo)
    // -- no hay mas concepto de dP invertida/alarma con este dato. frac usa el
    // rango completo de uint16 solo para tener una barra que se mueva.
    float frac = (float)r.presionRaw16 / 65535.0f;

    uint16_t emptyColor = _tft.color565(90, 90, 90);
    uint16_t fillColor  = _tft.color565(200, 15, 15);

    // linea gruesa completa en "vacio" -- reservorio + tubo inclinado,
    // el empalme queda redondeado solo porque los circulos de punta de
    // cada tramo coinciden en el mismo punto
    _drawThickLine(0, TUBE_Y0, TUBE_X0, TUBE_Y0, TUBE_HALFW, emptyColor);
    _drawThickLine(TUBE_X0, TUBE_Y0, TUBE_X1, TUBE_Y1, TUBE_HALFW, emptyColor);

    // relleno rojo encima, hasta el nivel actual (reservorio siempre + el
    // tramo del inclinado hasta la fraccion de la lectura)
    int16_t lx = TUBE_X0 + (int16_t)((TUBE_X1 - TUBE_X0) * frac);
    int16_t ly = TUBE_Y0 + (int16_t)((TUBE_Y1 - TUBE_Y0) * frac);
    _drawThickLine(0, TUBE_Y0, TUBE_X0, TUBE_Y0, TUBE_HALFW, fillColor);
    _drawThickLine(TUBE_X0, TUBE_Y0, lx, ly, TUBE_HALFW, fillColor);

    char val[16];
    snprintf(val, sizeof(val), "%04X", r.presionRaw16);   // TEMPORAL: crudo en hex
    _tft.setTextColor(TFT_WHITE, TFT_BLACK);
    _tft.drawCentreString(val, 120, 300, 4);
}

// -------------------------------------------------------------
// _drawThickLine -- "capsula" rellena: rectangulo + circulo en cada
// punta. Al dibujar dos tramos que comparten un punto con el mismo
// halfWidth, los circulos de esa union coinciden exacto y se funden
// en un empalme redondeado sin costura.
// -------------------------------------------------------------
void DisplayManager::_drawThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                     int16_t halfWidth, uint16_t color) {
    float dx  = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) { _tft.fillCircle(x0, y0, halfWidth, color); return; }

    float ux = dx / len, uy = dy / len;
    float px = -uy, py = ux;   // perpendicular

    int16_t ax = x0 - px * halfWidth, ay = y0 - py * halfWidth;
    int16_t bx = x0 + px * halfWidth, by = y0 + py * halfWidth;
    int16_t cx = x1 + px * halfWidth, cy = y1 + py * halfWidth;
    int16_t ddx = x1 - px * halfWidth, ddy = y1 - py * halfWidth;

    _tft.fillTriangle(ax, ay, bx, by, cx, cy, color);
    _tft.fillTriangle(ax, ay, cx, cy, ddx, ddy, color);
    _tft.fillCircle(x0, y0, halfWidth, color);
    _tft.fillCircle(x1, y1, halfWidth, color);
}

// -------------------------------------------------------------
// _drawDegC -- "<valor>" + circulito de grados + "C", alineado a
// izquierda desde (x,y). El simbolo grado se dibuja a mano porque no
// esta confirmado que el byte de fuente correspondiente exista en las
// fuentes embebidas de TFT_eSPI -- un circulo hueco es seguro siempre.
// -------------------------------------------------------------
int16_t DisplayManager::_drawDegC(int8_t value, int16_t x, int16_t y,
                                   uint8_t font, uint16_t color) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", value);

    _tft.setTextColor(color, TFT_BLACK);
    _tft.drawString(buf, x, y, font);
    int16_t numW = _tft.textWidth(buf, font);

    int16_t r  = (font >= 4) ? 3 : 2;
    int16_t cx = x + numW + r + 1;
    int16_t cy = y + r;
    _tft.drawCircle(cx, cy, r, color);

    int16_t cEndX = cx + r + 2;
    _tft.drawString("C", cEndX, y, font);
    return cEndX + _tft.textWidth("C", font);
}
