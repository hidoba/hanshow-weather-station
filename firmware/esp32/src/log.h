#pragma once
// Logging to USB serial that never blocks: the ESP32-S3 USB CDC stalls writers once its
// buffer is full and nobody on the host side is reading, so drop messages instead.
#include <Arduino.h>

inline void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
inline void logf(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  n = min(n, (int)sizeof buf - 1);
  if (n > 0 && Serial.availableForWrite() >= n) Serial.write((const uint8_t *)buf, n);
}

inline void logln(const String &s) { logf("%s\n", s.c_str()); }
