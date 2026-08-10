#include "Display.h"
#include "gateway_pinout.h"
#include <math.h>
#include <string.h>
#include <stdint.h>

// =============================================================
// Display.cpp
// =============================================================

// Formatea siempre con signo explicito ("+12,34" / "-5,00"), nunca solo "-"
// implicito del %d para los negativos y nada para los positivos.
static void formatSignedHundredths(int16_t hundredths, char *out, size_t outSize, const char *suffix) {
    char    sign  = (hundredths < 0) ? '-' : '+';
    int16_t whole = abs(hundredths);
    snprintf(out, outSize, "%c%d,%02d%s", sign, whole / 100, whole % 100, suffix);
}

// -- layout modo Tabla --
#define STATUS_H      20
#define ROW_H         20   // (320-STATUS_H)/NUM_NODES = 300/15 = 20 exacto

// -- columnas de la barra de estado -- 3 zonas independientes, cada una se
// redibuja sola (ver _drawStatusBar()) sin tocar las otras
#define COL_DATE_X    4
#define COL_CLOCK_X   58
#define COL_CLOCK_W   80
#define COL_QUEUE_X   142   // hueco libre entre el reloj (termina en 138) y la "I" (empieza en 222)
#define COL_QUEUE_W   72
#define COL_I_X       222

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
    _debugFreeze    = false;
    _mode           = MODE_TABLA;
    _wasTouched     = false;
    _lastTouchMs    = 0;
    _selectedNode   = 0;
    _queueCount     = 0;
    _internetOk     = false;   // sin TX intentada todavia
    _maxRedrawMs = 0;
    _day = _month = _year = _hour = _minute = _second = 0;
    _needFullRedraw = false;
    _dateDirty      = false;
    _clockDirty     = false;
    _internetDirty  = false;
    _queueDirty     = false;
    _dataDirty      = false;
    memset(_rowDirty, 0, sizeof(_rowDirty));

    // arranca en sentinel ("sin dato"), no en ceros crudos -- 0 es un valor
    // valido en presion/temp/vbat, memset(0) haria que valid() diera true
    // con datos falsos hasta que llegue el primer frame real
    for (uint8_t i = 0; i < NUM_NODES; i++) {
        _readings[i].presionMmH2O = SENTINEL_INT16;
        _readings[i].tempC        = SENTINEL_INT8;
        _readings[i].vbat         = SENTINEL_INT8;
    }
}

void DisplayManager::showBootMessage(const char *msg) {
    digitalWrite(PIN_TFT_BL, HIGH);
    _tft.fillScreen(TFT_BLACK);

    _tft.setTextColor(TFT_WHITE, TFT_BLACK);
    _tft.drawCentreString("CleanSens", 120, 125, 4);
    _tft.drawFastHLine(60, 155, 120, _tft.color565(60, 60, 60));

    _tft.setTextColor(_tft.color565(160, 160, 160), TFT_BLACK);
    _tft.drawCentreString(msg, 120, 168, 2);
}

void DisplayManager::endBootMessage() {
    digitalWrite(PIN_TFT_BL, LOW);
    _tft.fillScreen(TFT_BLACK);
    // _displayOn queda en false (default de init()) -- el criterio "apagado
    // por defecto, on demand via touch" sigue igual despues del arranque
}

void DisplayManager::setReadings(const NodeReading readings[NUM_NODES]) {
    // _rowDirty por nodo se sigue marcando (lo usa MODE_GRAFICO para saber
    // si el nodo que se esta viendo cambio); _dataDirty es el disparador
    // simple para MODE_TABLA, que siempre redibuja completo si cambio algo.
    for (uint8_t i = 0; i < NUM_NODES; i++) {
        if (memcmp(&_readings[i], &readings[i], sizeof(NodeReading)) != 0) {
            _readings[i] = readings[i];
            _rowDirty[i] = true;
            _dataDirty   = true;
        }
    }
}

void DisplayManager::setClock(uint8_t day, uint8_t month, uint8_t year,
                               uint8_t hour, uint8_t minute, uint8_t second) {
    if (_day != day || _month != month || _year != year) {
        _day = day; _month = month; _year = year;
        _dateDirty = true;
    }
    if (_hour != hour || _minute != minute || _second != second) {
        _hour = hour; _minute = minute; _second = second;
        _clockDirty = true;
    }
}

void DisplayManager::setQueueCount(uint16_t count) {
    // 2026-08-06 -- reactivado: muestra el backlog real de DataPost
    // (dataPost_pendientes()) en la barra de estado.
    if (_queueCount != count) {
        _queueCount = count;
        _queueDirty = true;
    }
}

void DisplayManager::setInternetOk(bool ok) {
    if (_internetOk != ok) {
        _internetOk    = ok;
        _internetDirty = true;
    }
}

void DisplayManager::showPostFail(int code, const char *resp) {
    _displayOn   = true;
    _debugFreeze = true;
    digitalWrite(PIN_TFT_BL, HIGH);

    _tft.fillScreen(TFT_BLACK);
    _tft.setTextColor(TFT_RED, TFT_BLACK);
    _tft.drawCentreString("TX FALLO", 120, 8, 4);

    char buf[24];
    snprintf(buf, sizeof(buf), "HTTP code: %d", code);
    _tft.setTextColor(TFT_WHITE, TFT_BLACK);
    _tft.drawString(buf, 4, 48, 2);
    _tft.drawString("Respuesta:", 4, 70, 2);

    // wrap manual por caracteres -- ~34 caracteres entran en 232px a font 1
    const uint8_t FONT = 1;
    const int     CHARS_POR_LINEA = 34;
    int len = strlen(resp);
    int y = 90;
    for (int i = 0; i < len && y < 300; i += CHARS_POR_LINEA) {
        char linea[CHARS_POR_LINEA + 1];
        int n = (len - i < CHARS_POR_LINEA) ? (len - i) : CHARS_POR_LINEA;
        memcpy(linea, resp + i, n);
        linea[n] = '\0';
        _tft.drawString(linea, 4, y, FONT);
        y += 10;
    }

    _tft.setTextColor(_tft.color565(120, 120, 120), TFT_BLACK);
    _tft.drawCentreString("(tocar para volver)", 120, 305, 2);
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

    if (_debugFreeze) {
        // pantalla congelada por showPostFail() -- ignora toda la maquina de
        // estados normal (navegacion, timeouts de apagado/grafico) hasta el
        // primer toque, que descongela y fuerza volver a MODE_TABLA.
        if (tapEdge) {
            _debugFreeze    = false;
            _mode           = MODE_TABLA;
            _needFullRedraw = true;
            _lastTouchMs    = now;
        }
        return;
    }

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

    // redibujo dirigido por cambios reales, no por un timer parejo -- solo
    // redibuja si de verdad hay algo nuevo (touch, frame o status). En
    // MODE_TABLA hay dos niveles: la franja de arriba se redibuja sola
    // (_drawStatusBar(), 2026-08-05: partida en 3 zonas independientes --
    // fecha/hora/"I" -- cada una repinta solo su propio rectangulo, ver
    // adentro de _drawStatusBar()). La pantalla completa (con las 15 filas)
    // solo se redibuja cuando cambian datos de nodos (_dataDirty, ~1x/min,
    // cuando llega el frame agregado) o hay wake/cambio de modo
    // (_needFullRedraw) -- asi el titileo de pantalla completa queda a esa
    // cadencia de un minuto, no de un segundo. Cada rama que efectivamente
    // dibuja se mide con millis() y actualiza _maxRedrawMs -- el peor caso
    // observado en banco (loop() queda bloqueado ese tiempo, asi que es el
    // numero que importa para saber si compite con algo mas, ej. el
    // handoff de datos al nucleo de WiFi).
    unsigned long t0 = millis();
    bool didDraw = true;

    if (_mode == MODE_TABLA) {
        if (_needFullRedraw || _dataDirty) {
            _drawTablaFull();   // ya incluye _drawStatusBar() adentro (fuerza las 3 zonas, ver ahi)
            _needFullRedraw = false;
            _dataDirty      = false;
            memset(_rowDirty, 0, sizeof(_rowDirty));
        } else if (_dateDirty || _clockDirty || _internetDirty || _queueDirty) {
            _drawStatusBar();   // redibuja solo las zonas que cambiaron, limpia sus propios flags
        } else {
            didDraw = false;
        }
    } else {   // MODE_GRAFICO
        bool selectedDirty = _rowDirty[_selectedNode];
        if (_needFullRedraw || selectedDirty) {
            _drawGrafico();
            _needFullRedraw = false;
        } else {
            didDraw = false;
        }
        // se consumen igual aunque no esten a la vista -- no hace falta
        // acarrearlos: el cambio de modo de vuelta a TABLA ya fuerza
        // _needFullRedraw=true (ver timeout mas arriba), asi que la tabla
        // sale al dia sin depender de estos flags.
        memset(_rowDirty, 0, sizeof(_rowDirty));
        _dataDirty = false;
    }

    if (didDraw) {
        uint32_t dt = (uint32_t)(millis() - t0);
        if (dt > _maxRedrawMs) _maxRedrawMs = dt;
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
    // fillScreen borro las 4 zonas de la barra de estado -- forzar que
    // _drawStatusBar() las repinte todas, sin importar si habian cambiado
    // de verdad (si no, quedarian en negro/invisibles tras el fillScreen)
    _dateDirty = _clockDirty = _internetDirty = _queueDirty = true;
    _drawStatusBar();
    _drawRows();
}

// -------------------------------------------------------------
// _drawStatusBar -- franja de arriba, 4 zonas independientes (fecha/hora/
// backlog de cola/"I"), cada una repinta SOLO su propio rectangulo -- evita
// repintar (y titilar) partes que no cambiaron.
// -------------------------------------------------------------
void DisplayManager::_drawStatusBar() {
    if (_dateDirty) {
        _tft.fillRect(COL_DATE_X, 0, COL_CLOCK_X - COL_DATE_X, STATUS_H - 1, TFT_BLACK);
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d/%02d", _day, _month);
        _tft.setTextColor(TFT_WHITE, TFT_BLACK);
        _tft.drawString(buf, COL_DATE_X, 3, 2);
        _dateDirty = false;
    }

    if (_clockDirty) {
        _tft.fillRect(COL_CLOCK_X, 0, COL_CLOCK_W, STATUS_H - 1, TFT_BLACK);
        char buf[12];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", _hour, _minute, _second);
        _tft.setTextColor(TFT_WHITE, TFT_BLACK);
        _tft.drawString(buf, COL_CLOCK_X, 3, 2);
        _clockDirty = false;
    }

    if (_queueDirty) {
        _tft.fillRect(COL_QUEUE_X, 0, COL_QUEUE_W, STATUS_H - 1, TFT_BLACK);
        // "C" de "Cola" -- "P" ya significa presion en este proyecto (campo
        // "p" del JSON, columnas P_1..P_15 en Sheets), usarlo aca confundiria.
        // Solo se muestra si hay backlog -- en operacion normal (cola vacia)
        // no vale la pena ocupar espacio con "C:0" todo el tiempo.
        if (_queueCount > 0) {
            char buf[10];
            snprintf(buf, sizeof(buf), "C:%u", (unsigned)_queueCount);
            _tft.setTextColor(TFT_YELLOW, TFT_BLACK);
            _tft.drawString(buf, COL_QUEUE_X, 3, 2);
        }
        _queueDirty = false;
    }

    if (_internetDirty) {
        _tft.fillRect(COL_I_X, 0, 240 - COL_I_X, STATUS_H - 1, TFT_BLACK);
        _tft.setTextColor(_internetOk ? TFT_GREEN : TFT_RED, TFT_BLACK);
        _tft.drawString("I", COL_I_X, 3, 2);
        _internetDirty = false;
    }

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

        const char *presionStr = "--,--mm";   // mismo ancho/forma que un valor real con signo
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
        formatSignedHundredths(r.presionMmH2O, buf, sizeof(buf), "mm");
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

    // r.presionMmH2O ya viene en centesimas de mmH2O, con signo -- frac mapea
    // el rango +-SENSOR_RANGE_MM (0 = vacio, extremo negativo; 1 = lleno,
    // extremo positivo; 0,5 = 0mm, sin diferencial), clampeado por si el
    // clamp del nodo (+-9999) quedara apenas afuera de SENSOR_RANGE_MM*100.
    const float rangeCenti = (float)SENSOR_RANGE_MM * 100.0f;
    float frac = ((float)r.presionMmH2O + rangeCenti) / (2.0f * rangeCenti);
    if (frac < 0.0f) frac = 0.0f;
    else if (frac > 1.0f) frac = 1.0f;

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
    formatSignedHundredths(r.presionMmH2O, val, sizeof(val), " mmH2O");
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
