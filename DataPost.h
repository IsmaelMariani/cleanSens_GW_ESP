// 2026-09-30 19:44 - Evento boot (motivo de reset + contador) a GAS
// 2026-09-30 19:23 - Reescrito: ventanas de radio, reintento, anillo
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
// El POST corre en una task de FreeRTOS pinneada a Core 0 -- nunca en el
// loop() de Core 1 (picLink_poll()/fino/segundero, timing vital con el PICG).
// La task NO TOCA el WiFi: solo manda cuando WifiPower.h dice que hay una
// ventana abierta con el radio conectado (wifiPower_enlaceListo()).
//
// 2026-09-30 -- COLA Y REINTENTOS (sin tocar GAS):
//   * Buffer circular en RAM (heap) de DATAPOST_QUEUE_LEN registros (576 =
//     ~6 dias a 15 min). Si se llena, se descarta el MAS VIEJO.
//   * Un registro sale de la cola SOLO si se da por escrito:
//       - HTTP 302: el script de GAS termino sin excepcion no capturada.
//       - -11 (HTTPC_ERROR_READ_TIMEOUT): el POST ya salio; GAS sigue
//         ejecutando y graba igual aunque el cliente corte a los 20 s
//         (observado en banco, el doPost puede tardar hasta ~200 s). Se da
//         por entregado -- reintentarlo duplicaria la fila.
//   * Cualquier otro resultado se reintenta en la PROXIMA ventana (nunca en
//     la misma, para no superponer ejecuciones de doPost):
//       - el POST nunca salio (radio sin conectar, DNS, TLS, conexion
//         rechazada): reintento sin limite hasta que la cola lo desplace.
//       - conexion cortada a mitad de envio (-3/-5): ambiguo -- puede haber
//         llegado; se reintenta y se cuenta en dataPost_ambiguos() (riesgo
//         residual: una fila duplicada, nunca un dato perdido).
//       - GAS respondio algo que no es 302: se cuenta un fallo por registro;
//         a los DATAPOST_MAX_FALLOS_HTTP se descarta (error permanente, no
//         debe trabar la cola).
//   * Tope de DATAPOST_MAX_POR_VENTANA envios por ventana, y un fallo corta
//     el envio del resto de esa ventana.
// =============================================================

void dataPost_init();      // reserva la cola + crea la task de envio (Core 0) -- llamar 1x en setup()

// no bloqueante -- llamar desde loop() (Core 1) cuando cierra un bloque.
// epochMedidoMs: instante de cierre del bloque (clave unica del registro, ver
// .ino), 0 si no hay hora real (manda "fyhMedido":null -> 1970-01-01 en la
// hoja, marca de dato de prueba o sin sync).
void dataPost_encolar(const NodeReading readings[NUM_NODES], int64_t epochMedidoMs);

// 2026-09-30: evento de arranque. Se encola en setup() con el motivo del reset
// (esp_reset_reason_t) y el contador de reinicios (persistido en flash por el
// .ino). Viaja por la MISMA cola y con las mismas reglas de entrega que las
// mediciones. JSON: {"tipo":"evento","ev":"boot","fyh":<epoch ms del boot o
// null>,"motivo":"PANIC","reinicios":N,"fw":"..."}. El doPost() de GAS tiene
// que tener una rama tipo "evento" que lo grabe (hoja Log_Eventos de
// PresionDif_JIM); con un GAS sin esa rama el evento se da por entregado (302)
// y se pierde en silencio. fyh: se calcula al ENVIAR (hora real - millis()), asi
// es el instante real del boot aunque el envio se demore; el envio espera hasta
// 30 s a que haya hora real (NTP) tras el arranque.
void dataPost_encolarBoot(uint8_t resetReason, uint32_t reinicios);

// -- handshake con WifiPower.cpp (Core 0) --
void dataPost_nuevaVentana();   // abre una ventana: rearma el tope de envios y el corte por fallo
bool dataPost_inactivo();       // true si ya no va a mandar nada mas en esta ventana (cola vacia, tope o fallo) y no hay POST en vuelo
bool dataPost_enVuelo();        // true mientras hay un POST en curso

// resultado del ultimo envio intentado (true = dado por escrito: 302 o -11)
bool dataPost_ultimoOk();

// si el ultimo intento encontro WiFi ya conectado
bool dataPost_ultimoWifiOk();

// incrementa 1x por intento de envio -- para que loop() detecte "hay resultado nuevo"
uint32_t dataPost_resultSeq();

// HTTP code crudo y fragmento (119 chars) del body de la ultima respuesta
int dataPost_ultimoCode();
const char *dataPost_ultimoResp();

// cuantos registros hay esperando en la cola ahora mismo
uint16_t dataPost_pendientes();

// contadores desde el boot
uint32_t dataPost_perdidos();    // descartados: cola llena o GAS rechazo DATAPOST_MAX_FALLOS_HTTP veces
uint32_t dataPost_entregados();  // dados por escrito (302 + timeouts)
uint32_t dataPost_timeouts();    // de los entregados, cuantos fueron -11
uint32_t dataPost_ambiguos();    // cortes a mitad de envio (-3/-5), reintentados
