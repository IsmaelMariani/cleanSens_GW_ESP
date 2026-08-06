#pragma once

// =============================================================
// GasConfig.h
//
// URL unica del Web App de Apps Script (proyecto standalone en Codigo/) --
// doGet() (config WiFi, ver WifiConfig.cpp) y doPost() (ingesta de datos,
// ver DataPost.cpp) viven en el MISMO proyecto/deployment, Apps Script
// despacha segun el metodo HTTP. Un solo lugar para esta URL evita el bug
// de actualizarla en un archivo y olvidarla en el otro.
//
// Recordar (ya paso 2 veces, ver notas del proyecto): editar y guardar el
// codigo del script NO alcanza -- hay que Implementar -> Gestionar
// implementaciones -> "Nueva version" -> Implementar para que esta URL
// sirva el codigo actualizado. La URL en si no cambia con eso.
// =============================================================

#define GAS_WEBAPP_URL  "https://script.google.com/macros/s/AKfycbzapPki_5bT4fp5XFxluztS6eM1UYylq1jZZ_r_xkzzab8Cl6_xAg0r0uBrYoK1kEZP/exec"
