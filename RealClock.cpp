// 2026-09-30 19:23 - resincronizar() por ventana de radio
#include "RealClock.h"
#include <Arduino.h>
#include <time.h>
#include "esp_sntp.h"

// =============================================================
// RealClock.cpp -- ver RealClock.h para el diseño.
// =============================================================

static volatile bool s_syncReciente = false;

// callback del cliente SNTP: se dispara cuando el servidor contesto y se ajusto la hora
static void onTimeSync(struct timeval *tv) {
    (void)tv;
    s_syncReciente = true;
}

void realClock_init() {
    sntp_set_time_sync_notification_cb(onTimeSync);
    configTime(REALCLOCK_GMT_OFFSET_SEC, 0, REALCLOCK_NTP_SERVER);
}

// configTime() reinicia el cliente SNTP, que manda un request de inmediato
void realClock_resincronizar() {
    s_syncReciente = false;
    configTime(REALCLOCK_GMT_OFFSET_SEC, 0, REALCLOCK_NTP_SERVER);
}

bool realClock_sincronizoReciente() {
    return s_syncReciente;
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
