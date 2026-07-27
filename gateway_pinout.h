#pragma once

// =============================================================
// gateway_pinout.h
// Pines propios del proyecto -- NO incluye pines de display
// (esos viven en User_Setup.h de TFT_eSPI y no se tocan)
// Placa: ESP32-2432S028R  (Shenzhen Jingcai / "CYD")
// =============================================================

// -- SD Card --
#define PIN_SD_CS       5

// -- Touch resistivo XPT2046 -- SPI por software, bus propio
// (no comparte MOSI con el display -- ausente en poller3_pinout.h,
// causa probable del touch inestable en Poller 3.0)
#define PIN_TOUCH_CLK   25
#define PIN_TOUCH_CS    33
#define PIN_TOUCH_MOSI  32
#define PIN_TOUCH_MISO  39
#define PIN_TOUCH_IRQ   36

// -- UART PIC gateway --
// El PIC esta cableado al conector de alimentacion de la placa (P1 en el
// ESP32-2432S028R/CYD), que es UART0 (GPIO1/GPIO3 fijos) -- el mismo que
// el chip USB-serie de a bordo. Por eso PicLink.cpp usa "Serial" (UART0)
// directamente, no Serial2 -- no hay pines a elegir, y no hay forma de
// usar el Monitor Serie ni tener el USB conectado al mismo tiempo (ver
// PicLink.cpp para el detalle). Estas constantes quedan sin uso; si en el
// futuro se recablea a un UART aparte (GPIO16/17, Serial2), reactivarlas.

// -- Backlight display (control on/off, no es routing de TFT_eSPI) --
#define PIN_TFT_BL      21

// -- Reservados display ILI9341 (no usar) --
// GPIO 2  = TFT_DC
// GPIO 12 = TFT_RST
// GPIO 13 = TFT_MOSI
// GPIO 14 = TFT_SCLK
// GPIO 15 = TFT_CS
// GPIO 21 = TFT_BL
