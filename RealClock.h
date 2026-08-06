#pragma once

#include <stdint.h>

// =============================================================
// RealClock.h
//
// Hora real vía NTP (configTime(), API estandar del core ESP32 -- SNTP por
// debajo). Independiente del reloj local fino/segundero/minutero del .ino
// (ese sigue siendo el contador de 10ms usado para el protocolo con el
// PICG, no tiene relacion con la hora de pared).
//
// Una vez sincronizado, time()/localtime() (libc) siguen avanzando solos
// usando el timer de sistema del ESP32 -- no dependen de la red para seguir
// contando, incluyen el cruce de medianoche/mes/año correctamente (no hay
// aritmetica de calendario propia aca). El cliente SNTP interno reintenta
// solo cada ~1h para corregir el drift acumulado del cristal.
// =============================================================

#define REALCLOCK_GMT_OFFSET_SEC  (-3 * 3600)   // Argentina, GMT-3 fijo, sin horario de verano
#define REALCLOCK_NTP_SERVER      "pool.ntp.org"

void realClock_init();     // configTime() -- llamar 1x en setup(), despues de conectar WiFi
bool realClock_valido();   // true si ya hubo al menos 1 sync NTP exitoso

// Y/M/D/H/M/S reales -- solo tienen sentido si realClock_valido()==true,
// en caso contrario deja todos los campos en 0.
void realClock_obtener(uint8_t &day, uint8_t &month, uint8_t &year,
                        uint8_t &hour, uint8_t &minute, uint8_t &second);

// epoch actual en milisegundos, 0 si todavia no sincronizo -- para
// fyhMedido del POST (ver DataPost.cpp).
int64_t realClock_epochMs();
