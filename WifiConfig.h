// 2026-09-30 19:23 - Agrega iniciarConexion()/apagar() del radio
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
// Secuencia de conexion (wifiConfig_conectar(), 2026-08-10): intenta PRIMERO
// el fallback (celu, 1 intento -- solo esta presente en el momento de
// instalar, no vale la pena gastarle mas) y RECIEN DESPUES la activa (hasta
// WIFI_CONNECT_ATTEMPTS veces, la conexion real esperada en operacion). El
// celu es exclusivamente una ayuda de instalacion (el instalador la usa para
// llegar a internet y actualizar Config_WiFi/GAS con la red real del sitio,
// despues resetea la placa) -- nunca se vuelve a intentar en operacion
// normal, ver wifiConfig_mantener().
// =============================================================

void wifiConfig_init();      // siembra activa+fallback en flash si hace falta -- llamar 1x en setup(), antes de conectar
void wifiConfig_conectar();  // bloqueante: fallback/celu (1 intento) -> activa (hasta 3 intentos) si el celu no esta
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

// 2026-09-30: solo la llama WifiPower.cpp (task de Core 0) durante la ventana
// de instalacion del boot -- si esta desconectado, reintenta cada
// WIFI_RECONNECT_INTERVAL_MS con WiFi.begin() apuntando SIEMPRE a la
// credencial activa (NO bloqueante) -- nunca al celu. Ya no se llama desde
// loop().
void wifiConfig_mantener();

// 2026-09-30: control del radio para el ciclo de ventanas (WifiPower.cpp,
// unico que las llama). iniciarConexion(): prende el radio y empieza a
// conectar a la credencial activa, NO bloquea (quien llama pollea
// WiFi.status()). apagar(): apaga el radio de verdad.
void wifiConfig_iniciarConexion();
void wifiConfig_apagar();

// Solo debug -- credenciales actuales en flash, "ssid / pass" en texto
// plano. Solo debug (Serial, UART0; el PIC va por Serial2 desde 2026-09-30).
String wifiConfig_activaStr();
String wifiConfig_fallbackStr();
