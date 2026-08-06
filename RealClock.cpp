#include "RealClock.h"
#include <Arduino.h>
#include <time.h>

// =============================================================
// RealClock.cpp -- ver RealClock.h para el diseño.
// =============================================================

void realClock_init() {
    configTime(REALCLOCK_GMT_OFFSET_SEC, 0, REALCLOCK_NTP_SERVER);
}

bool realClock_valido() {
    struct tm ti;
    return getLocalTime(&ti, 0);   // timeout 0 -- chequeo inmediato, no bloquea
}

void realClock_obtener(uint8_t &day, uint8_t &month, uint8_t &year,
                        uint8_t &hour, uint8_t &minute, uint8_t &second) {
    struct tm ti;
    if (!getLocalTime(&ti, 0)) {
        day = month = year = hour = minute = second = 0;
        return;
    }
    day    = (uint8_t)ti.tm_mday;
    month  = (uint8_t)(ti.tm_mon + 1);
    year   = (uint8_t)(ti.tm_year % 100);
    hour   = (uint8_t)ti.tm_hour;
    minute = (uint8_t)ti.tm_min;
    second = (uint8_t)ti.tm_sec;
}

int64_t realClock_epochMs() {
    if (!realClock_valido()) return 0;
    return (int64_t)time(nullptr) * 1000LL;
}
