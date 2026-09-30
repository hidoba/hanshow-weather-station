#pragma once
#include <stdint.h>

// Native panel RAM layout: 250 rows (landscape x, reversed) of 16 bytes (landscape y, MSB = top).
#define EPD_ROW_BYTES   16
#define EPD_ROWS        250
#define EPD_PLANE_SIZE  (EPD_ROW_BYTES * EPD_ROWS)

extern uint8_t epd_black[EPD_PLANE_SIZE]; // 1 = white, 0 = black
extern uint8_t epd_red[EPD_PLANE_SIZE];   // 1 = red

uint8_t epd_detect(void);        // 1 if SSD1680-style BWR 2.13" panel detected
// Refresh from both planes, blocks until done; returns panel temperature.
// fast: ~1.5 s non-flashing update; red pixels only get the given phase-A drive byte.
uint8_t epd_refresh(uint8_t fast, uint8_t red_drive, uint8_t frames);

extern uint16_t epd_led_level; // green LED brightness while a refresh runs, 0.1 % units
void led_pwm(uint16_t level, int ms, uint32_t pin);  // level in 0.1 % units, blocking
