// 2026-09-30 19:23 - AVERAGER_INTERVAL_MIN = 15
#pragma once

#include "SharedTypes.h"

// =============================================================
// Averager.h
//
// Acumula las lecturas que llegan del PICG 1x/minuto (ROUND_CLOSED) y arma
// el promedio de presion por UTA para el POST periodico a GAS, cada
// AVERAGER_INTERVAL_MIN minutos -- valor fijo, se ajusta reflasheando
// cuando el cliente defina el intervalo real (no configurable remoto por
// ahora, decision explicita del usuario 2026-08-06).
//
// Promedio SOLO de presion, dividido por la cantidad real de lecturas
// validas de esa UTA en la ventana (no por AVERAGER_INTERVAL_MIN fijo) --
// asi 1, 2 o 3 rondas caidas de una UTA puntual no arrastran su promedio
// hacia abajo. temp/vbat/idNodo NO se promedian -- son diagnostico, no la
// medicion critica -- se manda el ultimo valor valido visto en la ventana.
// Si una UTA no tuvo NINGUNA lectura valida en toda la ventana, la fila
// viaja con esa UTA en sentinel (null en el JSON), mismo criterio que el
// resto del proyecto.
// =============================================================

#define AVERAGER_INTERVAL_MIN  15   // 2026-09-30: bloque unico de 15 min, media directa de las lecturas validas (definitivo)

void averager_init();

// llamar 1x por minuto, cuando el PICG cierra una ronda (ROUND_CLOSED) --
// acumula presion (solo UTAs con dato valido esa ronda) y guarda el ultimo
// temp/vbat/idNodo valido de cada una.
void averager_agregar(const NodeReading readings[NUM_NODES]);

// arma el promedio de la ventana en out[] y resetea los acumuladores para
// la proxima ventana -- llamar exactamente 1 vez por ciclo de TX.
void averager_calcularYresetear(NodeReading out[NUM_NODES]);
