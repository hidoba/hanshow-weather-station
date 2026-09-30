#pragma once
#include <stdint.h>
#include "app_config.h"

#define RXD         GPIO_PA0
#define TXD         GPIO_PB1

#define EPD_RESET   GPIO_PD4
#define EPD_DC      GPIO_PD7
#define EPD_BUSY    GPIO_PA1
#define EPD_CS      GPIO_PB4
#define EPD_CLK     GPIO_PB5
#define EPD_MOSI    GPIO_PB6
#define EPD_ENABLE  GPIO_PC5

#define LED_RED     GPIO_PD2
#define LED_GREEN   GPIO_PD3
