// Derived from atc1441/ATC_TLSR_Paper (https://github.com/atc1441/ATC_TLSR_Paper), MIT license:
// see firmware/tag/LICENSE-ATC_TLSR_Paper.
#include "swire.h"
#include "config.h"
#include <driver/gpio.h>

static HardwareSerial sws(2);

static void idle_high() {
  pinMode(TAG_SWS_PIN, OUTPUT);
  digitalWrite(TAG_SWS_PIN, HIGH);
}

void swire_begin() {
  // After deep sleep these pins are still held high. Configure them first and only then release
  // the hold, otherwise NRST briefly falls back to an undriven input and can reset the tag.
  pinMode(TAG_NRST_PIN, OUTPUT);  // driven high (low = reset)
  digitalWrite(TAG_NRST_PIN, HIGH);
  idle_high();
  pinMode(TAG_TX_PIN, OUTPUT);    // UART idle level until Serial1 takes the pin over
  digitalWrite(TAG_TX_PIN, HIGH);
  gpio_hold_dis((gpio_num_t)TAG_NRST_PIN);
  gpio_hold_dis((gpio_num_t)TAG_SWS_PIN);
  gpio_hold_dis((gpio_num_t)TAG_TX_PIN);
}

void swire_prepare_sleep() {
  digitalWrite(TAG_NRST_PIN, HIGH);
  digitalWrite(TAG_SWS_PIN, HIGH);
  Serial1.flush();                // let a just-sent command finish: end() discards pending bytes
  Serial1.end();                  // hand the UART TX pin over: idle level is high
  pinMode(TAG_TX_PIN, OUTPUT);
  digitalWrite(TAG_TX_PIN, HIGH);
  gpio_hold_en((gpio_num_t)TAG_NRST_PIN);  // floating lines reset / wake the tag
  gpio_hold_en((gpio_num_t)TAG_SWS_PIN);
  gpio_hold_en((gpio_num_t)TAG_TX_PIN);
  gpio_deep_sleep_hold_en();
}

void tag_hard_reset() {
  digitalWrite(TAG_NRST_PIN, LOW);
  delay(50);
  digitalWrite(TAG_NRST_PIN, HIGH);
}

// ---- SWire packet encoding ----

static int enc_byte(uint8_t *d, uint8_t b, bool first) {
  d[0] = first ? 0x80 : 0xFE;  // start bit: 1 for the command byte, 0 for data
  for (int i = 0; i < 8; i++) d[1 + i] = (b >> (7 - i)) & 1 ? 0x80 : 0xFE;
  d[9] = 0xFE;
  return 10;
}

static void wr(uint32_t addr, const uint8_t *data, size_t n) {
  static uint8_t pkt[(5 + 4 + 256 + 1) * 10];
  const uint8_t hdr[5] = {0x5A, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, 0x00};
  int k = 0;
  for (int i = 0; i < 5; i++) k += enc_byte(pkt + k, hdr[i], i == 0);
  for (size_t i = 0; i < n; i++) k += enc_byte(pkt + k, data[i], false);
  memset(pkt + k, 0x80, 9);  // end marker 0xff
  pkt[k + 9] = 0xFE;
  k += 10;
  sws.write(pkt, k);
}

static void wr1(uint32_t addr, uint8_t v) { wr(addr, &v, 1); }
static void wr2(uint32_t addr, uint8_t a, uint8_t b) { const uint8_t d[2] = {a, b}; wr(addr, d, 2); }

static void flash_cmd(uint8_t c) { wr1(0x0D, 0x00); wr2(0x0C, c, 0x01); }  // cns low, cmd + cns high
static void write_enable() { flash_cmd(0x06); }
static void soft_reset() { wr1(0x06F, 0x20); sws.flush(); }

static void unlock() {
  write_enable();
  wr1(0x0D, 0x00); wr1(0x0C, 0x01); wr2(0x0C, 0x00, 0x01);
  write_enable();
  wr1(0x0D, 0x00); wr1(0x0C, 0x01); wr1(0x0C, 0x00); wr2(0x0C, 0x00, 0x01);
}

static void sector_erase(uint32_t a) {
  write_enable();
  wr1(0x0D, 0x00); wr1(0x0C, 0x20); wr1(0x0C, a >> 16); wr1(0x0C, a >> 8); wr2(0x0C, a & 0xFF, 0x01);
  sws.flush();
  delay(300);
}

static void write_block(uint32_t a, const uint8_t *d, size_t n) {
  uint8_t blk[4 + 256] = {0x02, (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a};
  memcpy(blk + 4, d, n);
  write_enable();
  wr1(0x0D, 0x00);
  wr1(0x00B3, 0x80);  // SWS fifo mode: all bytes to one register
  wr(0x0C, blk, n + 4);
  wr1(0x00B3, 0x00);
  wr1(0x0D, 0x01);
  sws.flush();
  delay(10);
}

bool swire_halt_cpu() {
  sws.setTxBufferSize(4096);
  sws.begin(460800, SERIAL_8N1, -1, TAG_SWS_PIN);
  tag_hard_reset();
  soft_reset();
  uint32_t t0 = millis();
  while (millis() - t0 < 1000) wr1(0x0602, 0x05);  // CPU stop, repeated through the boot
  wr1(0x00B2, 55);                                  // SWS speed
  wr1(0x0602, 0x05);
  flash_cmd(0xAB);                                  // flash wake-up
  sws.flush();
  return true;
}

void swire_release() {
  soft_reset();
  sws.end();
  idle_high();
}

bool swire_flash(const uint8_t *fw, size_t n, void (*progress)(int)) {
  if (n < 16 || memcmp(fw + 8, "KNLT", 4)) return false;
  swire_halt_cpu();
  write_enable();
  unlock();
  sws.flush();
  delay(1500);
  for (size_t a = 0; a < n; a += 256) {
    if ((a & 0xFFF) == 0) sector_erase(a);
    write_block(a, fw + a, min((size_t)256, n - a));
    if (progress) progress(a * 100 / n);
  }
  swire_release();
  return true;
}

