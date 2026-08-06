#pragma once

#include <stdint.h>
#include <WString.h>

// =============================================================
// WifiConfig.h
//
// Dos credenciales en flash (Preferences, namespace "wifi"):
//   - "ssid"/"pass"     -- ACTIVA: la red real del sitio. Se actualiza sola
//                          desde PresionDif_JIM/Config_WiFi (ver
//                          wifiConfig_actualizarDesdeSheet()).
//   - "ssidFb"/"passFb" -- FALLBACK: hotspot fijo del celu del usuario.
//                          Sembrada una sola vez en el primer boot tras
//                          flashear, junto con la activa (mismos valores al
//                          principio) -- de ahi en mas nunca se vuelve a
//                          tocar por codigo, solo la activa se actualiza.
//
// Secuencia de conexion (wifiConfig_conectar()): intenta la activa hasta
// WIFI_CONNECT_ATTEMPTS veces: si agota los intentos sin conectar, cae a la
// de fallback. Resuelve el huevo-y-gallina de bootstrap (el ESP32 no puede
// bajar la config real de WiFi si todavia no tiene WiFi) -- el fallback
// siempre lleva a internet.
// =============================================================

void wifiConfig_init();      // siembra activa+fallback en flash si hace falta -- llamar 1x en setup(), antes de conectar
void wifiConfig_conectar();  // bloqueante: activa (hasta 3 intentos) -> fallback (1 intento) si la activa no prendio
bool wifiConfig_conectado(); // WL_CONNECTED?
String wifiConfig_ipStr();   // IP actual como texto, "" si no conectado -- solo para debug

// Consulta PresionDif_JIM/Config_WiFi (B2=ssid, B3=pass, B4=flag S/N) via
// GAS. Si el flag viene en "S", el propio doGet() de GAS ya lo resetea a
// "N" del lado servidor (lectura+consumo en un solo request, evita el caso
// de olvidarse el segundo llamado) y esta funcion graba ssid/pass nuevos en
// la credencial ACTIVA de flash. Si el flag viene en "N" (o falla la
// consulta), no toca nada. Devuelve true si actualizo -- normalmente no se
// llama sola, ver wifiConfig_bootstrap().
bool wifiConfig_actualizarDesdeSheet();

// Punto de entrada recomendado para setup() (test aislado o produccion):
// conecta + chequea actualizacion + si hubo update prueba la credencial
// nueva al toque (no espera al proximo boot) -- ver comentario grande en
// WifiConfig.cpp para la secuencia completa.
void wifiConfig_bootstrap();

// Llamar 1x por vuelta de loop() (produccion) -- si esta desconectado,
// reintenta cada WIFI_RECONNECT_INTERVAL_MS con WiFi.reconnect() (NO
// bloqueante, reusa la ultima credencial que se probo -- activa o
// fallback). No repite el bootstrap completo (eso bloquearia loop() varios
// segundos y arruinaria el timing de picLink_poll() con el PICG).
void wifiConfig_mantener();

// Solo debug -- credenciales actuales en flash, "ssid / pass" en texto
// plano. Nunca llamar con el PIC conectado (comparte Serial/UART0).
String wifiConfig_activaStr();
String wifiConfig_fallbackStr();
