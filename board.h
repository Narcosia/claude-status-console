// Waveshare ESP32-S3-Touch-AMOLED-1.75 board pins.
//
// Self-contained rather than including Waveshare's Mylibrary/pin_config.h,
// because that header has one wrong define worth not inheriting: it sets
// I2S_MCK_IO to 16, while the board schematic and the maintained BSP put audio
// MCLK on GPIO 42 and leave GPIO 16 as a free expansion pin. GPIO 16 is where
// the LED ring lives here, so the mistaken define is exactly the one that
// would matter.
//
// Source: HARDWARE_REFERENCE.md in waveshareteam/ESP32-S3-Touch-AMOLED-1.75.

#pragma once

// --- CO5300 AMOLED, 466x466, QSPI -------------------------------------------
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 38
#define LCD_CS 12
#define LCD_RESET 39
#define LCD_WIDTH 466
#define LCD_HEIGHT 466

// The AMOLED has no GPIO backlight line. Brightness is a panel command, so
// gfx->setBrightness() is the only control - LCD backlight PWM recipes from
// other boards do not apply here.

// --- CST9217 capacitive touch, on the shared I2C bus ------------------------
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 11
#define TP_RESET 40
#define TP_ADDR 0x5A

// Display reset (39) and touch reset (40) are separate lines. Resetting one
// does not reset the other.

// --- Expansion header H2 ----------------------------------------------------
//
//   pin 1  VBUS       5 V from USB - ring VCC
//   pin 2  GND        ring Gnd
//   pin 3  3V3
//   pin 4  GPIO44 / U0RXD
//   pin 5  GPIO43 / U0TXD
//   pin 6  GPIO16     <- see below
//   pin 7  GPIO17     <- see below
//   pin 8  GPIO18     ring Din
//
// All GPIO here is 3.3 V and NOT 5 V tolerant. VBUS is a power rail, not a
// logic reference.
//
// The GPIO order above is MEASURED, and contradicts HARDWARE_REFERENCE.md,
// which lists pins 6/7/8 as GPIO17/GPIO18/GPIO16. That non-sequential ordering
// is what gives it away: the pins actually run 16, 17, 18 across holes 6, 7, 8.
//
// Established with POST /ringscan, which drives each candidate pin in its own
// colour - GPIO16 red, GPIO17 green, GPIO18 blue - and lets the ring name its
// own pin. A ring in hole 8 lit blue. Trust that over the table; it is the
// second place this board's documentation disagrees with the hardware, after
// the I2S_MCK_IO note at the top of this file.
#define RING_PIN 18
