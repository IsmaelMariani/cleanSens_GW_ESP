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
// vuelve al toque.
//
// 2026-08-06: buffer circular en RAM (no SD) de DATAPOST_QUEUE_LEN
// registros (~2 dias a intervalo minimo, ver DataPost.cpp) -- si se corta
// internet, los promedios se siguen acumulando en la cola en vez de
// perderse; si la cola se llena (corte mas largo que eso), se descarta el
// MAS VIEJO para hacer lugar al nuevo (siempre se conservan los ultimos
// ~2 dias).
//
// 2026-08-07: la task de Core 0 YA NO reintenta un envio fallido -- lo
// descarta y sigue con el siguiente de la cola (ver dataPost_perdidos()).
// Antes reintentaba el mismo cada DATAPOST_RETRY_DELAY_MS hasta que salia
// bien, pero si GAS tardaba mas que el timeout del cliente (140s vistos en
// banco contra 20s de timeout) el reintento terminaba disparando una
// segunda ejecucion de doPost() que tambien escribia la fila, duplicando el
// dato en Lecturas/Ledger. Se acepta perder esa ronda puntual por ahora
// para medir cuan seguido pasa antes de decidir si hace falta logica de
// verificacion contra tipo=ultima.
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

// diagnostico 2026-08-06 -- HTTP code crudo y fragmento (119 chars) del body
// de la ultima respuesta -- para mostrar en pantalla si la TX fallo (ver
// DisplayManager::showPostFail() / .ino), sin Serial disponible en banco.
int dataPost_ultimoCode();
const char *dataPost_ultimoResp();

// cuantos registros hay esperando en la cola ahora mismo (no consume nada,
// solo lee) -- util para mostrar en pantalla si hay backlog acumulado por
// un corte de internet.
uint16_t dataPost_pendientes();

// cuantos envios se descartaron tras fallar (2026-08-07) -- contador interno
// desde el boot, sin mostrar en pantalla todavia. Sirve para medir cuan
// seguido pasa esto antes de decidir si hace falta logica de verificacion
// (GET a tipo=ultima) antes de reintentar en vez de descartar directo.
uint32_t dataPost_perdidos();

// true si la task de Core 0 detecto que WiFi.status() dice "conectado" pero
// ni siquiera resuelve DNS (router colgado) y quiere que Core1 fuerce una
// reconexion -- se consume solo (vuelve a false) al leerla. Llamar desde
// wifiConfig_mantener() (unico lugar que puede tocar el WiFi), nunca desde
// esta misma task.
bool dataPost_debeForzarReconexion();
