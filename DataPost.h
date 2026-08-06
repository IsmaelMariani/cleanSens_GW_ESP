#pragma once

#include "SharedTypes.h"
#include <stdint.h>

// =============================================================
// DataPost.h
//
// Arma y manda el POST de datos a GAS (mismo Web App que WifiConfig, ver
// GasConfig.h). JSON: {"tipo":"medicion","fyhMedido":<epoch ms o null>,
// "n":[...15],"p":[...15],"t":[...15],"v":[...15]}, agrupado por campo.
// UTA ausente (!readings[i].valid()) manda null en esa posicion de los 4
// arrays, no se omite -- la posicion (indice = UTA-1) es la unica
// referencia de que UTA es cada dato. Contrato verificado contra el
// doPost()/procesarMedicion() real (2026-08-05).
//
// El POST en si corre en una task de FreeRTOS pinneada a Core 0 -- nunca en
// el loop() de Core 1 (picLink_poll()/fino/segundero, timing vital con el
// PICG). La task NO TOCA el WiFi -- no conecta, no desconecta, no cambia de
// modo -- solo revisa WiFi.status() y manda si ya esta conectado. Mantener
// la conexion (wifiConfig_conectar()/wifiConfig_mantener()) sigue siendo
// 100% responsabilidad de loop()/Core 1 -- separar "quien mantiene la
// conexion" de "quien manda el dato" evito el bug de reconexion de un
// primer intento de este mismo diseño (mezclaba las dos cosas en la task).
//
// WiFi queda SIEMPRE conectado (no se apaga entre TX) -- se probo bajar
// consumo apagando el radio y resulto poco confiable en banco. Ahorro de
// bajo consumo queda pendiente para retomar mas adelante, con mas margen
// de testeo.
//
// dataPost_encolar() -- unica funcion que se llama desde loop() -- copia
// readings[] a una cola FreeRTOS (xQueueSend timeout 0, nunca bloquea) y
// vuelve al toque. Si la cola esta llena (una TX anterior todavia en
// vuelo) esta ronda se descarta -- mismo criterio de "sin cola/reintento
// persistente todavia" ya aceptado (SD pendiente, ver notas del proyecto
// 2026-07-16).
// =============================================================

void dataPost_init();      // crea la queue + la task de envio (Core 0) -- llamar 1x en setup()

// no bloqueante -- llamar desde loop() (Core 1) cuando toque transmitir
// (cada 2 rondas = ~2min, ver .ino). epochMedidoMs: hora real (ver
// RealClock.h) de cuando se cerro esta ronda, 0 si todavia no hay sync NTP
// (manda "fyhMedido":null en ese caso).
void dataPost_encolar(const NodeReading readings[NUM_NODES], int64_t epochMedidoMs);

// resultado de la ultima TX intentada (true=exito, false=fallo o todavia
// ninguna) -- para el indicador "I" del display (ver .ino), a pedido
// explicito del usuario ("la I guarda el color segun si pudo TX o no") en
// vez de WiFi.status() en vivo.
bool dataPost_ultimoOk();

// si el ultimo ciclo encontro WiFi ya conectado -- separado de
// dataPost_ultimoOk() porque son 2 fallas distintas (no habia conexion vs.
// conecto pero el POST/GAS fallo), util para mostrar en pantalla cual de
// las dos paso (ver .ino).
bool dataPost_ultimoWifiOk();

// incrementa 1x por ciclo completo (exitoso o no) -- para que loop() detecte
// "hay resultado nuevo" sin tener que comparar los booleans de arriba.
uint32_t dataPost_resultSeq();
