#pragma once
#include <stdint.h>

// Dual-bank firmware update over UART. The boot ROM starts the image at 0x00000 or 0x20000
// whose byte 8 is 0x4B ('K' of "KNLT"), checking 0x00000 first.
int ota_begin(uint32_t size, uint32_t crc);             // 0 ok; erases the idle bank
int ota_write(uint32_t off, const uint8_t *p, int n);  // 0 ok; n <= 256, must not cross a 256 B page
int ota_finish(void);                                   // 0 ok: verified and switched; caller reboots
uint32_t ota_running_bank(void);
