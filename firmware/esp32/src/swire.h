#pragma once
// Low-level access to the tag's TLSR825x over its single-wire debug pin (SWS), driven by a
// UART TX at 460800 baud where each UART byte encodes one SWire bit (port of tools/tlsr_flash.py).
// Needs NRST to catch the CPU right after reset, before the tag firmware detaches SWS.
#include <Arduino.h>

void swire_begin();                  // idle: SWS held high, NRST released
void tag_hard_reset();               // pulse NRST
bool swire_halt_cpu();               // reset + stop the CPU during boot; SWS stays attached
void swire_release();                // soft-reset the halted CPU and return SWS to idle
// Write a Telink image to flash address 0 and reboot the tag.
bool swire_flash(const uint8_t *fw, size_t n, void (*progress)(int));
