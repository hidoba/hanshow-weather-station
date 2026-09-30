#include "log.h"
#include "tag.h"
#include "config.h"
#include "swire.h"

static uint16_t crc16(const uint8_t *p, int n, uint16_t c = 0xFFFF) {
  while (n--) {
    c ^= *p++ << 8;
    for (int i = 0; i < 8; i++) c = c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1;
  }
  return c;
}

uint32_t EpdCanvas::hash() const {
  return (uint32_t)crc16(black, PLANE) << 16 | crc16(red, PLANE);
}

void Tag::wake() {
  s_.updateBaudRate(9600);  // one 0x00 at 9600 baud: ~1 ms low, well past the tag's pad wake-up
  s_.write((uint8_t)0);
  s_.flush();
  s_.updateBaudRate(115200);
  delay(5);  // crystal start-up
  while (s_.available()) s_.read();
}

String Tag::cmd(char c, const uint8_t *p, int n, uint32_t timeout_ms) {
  if (!last_io_ || millis() - last_io_ > 150) wake();  // the tag suspends after 300 ms idle
  uint8_t hdr[5] = {0xA5, 0x5A, (uint8_t)c, (uint8_t)n, (uint8_t)(n >> 8)};
  uint16_t crc = crc16(p, n, crc16(hdr + 2, 3));
  while (s_.available()) s_.read();
  s_.write(hdr, 5);
  if (n) s_.write(p, n);
  s_.write((uint8_t)crc);
  s_.write((uint8_t)(crc >> 8));
  String line;
  uint32_t t0 = millis();
  last_io_ = millis();
  if (!timeout_ms) {  // fire and forget: but make sure it's on the wire before we might sleep
    s_.flush();
    return "SENT";
  }
  while (millis() - t0 < timeout_ms) {
    if (!s_.available()) { delay(1); continue; }
    char ch = s_.read();
    if (ch != '\n') { line += ch; continue; }
    line.trim();
    last_io_ = millis();
    if (line.startsWith("OK") || line.startsWith("ER")) return line;
    line = "";  // boot chatter etc.
  }
  last_io_ = 0;  // no answer: wake it again next time
  return "TIMEOUT";
}

bool Tag::ping(String *info, int *refreshes) {
  String r = cmd('P', nullptr, 0, 500);
  if (info) *info = r;
  if (r.startsWith("OK P")) info_ = r;
  int i = r.indexOf("n=");
  if (refreshes) *refreshes = i >= 0 ? r.substring(i + 2).toInt() : -1;
  return r.startsWith("OK P");
}

bool Tag::show(const EpdCanvas &c, bool fast, uint16_t led, bool force_fast, bool wait) {
  uint32_t t0 = millis();
  int n = -1;
  bool alive = false;
  for (int i = 0; i < 10 && !(alive = ping(nullptr, &n)); i++) delay(300);  // tag may be booting
  if (!alive) {  // wedged: hard reset through NRST and give it one more chance
    logln("tag: not responding, resetting");
    st_.hard_resets++;
    tag_hard_reset();
    delay(2000);
    for (int i = 0; i < 10 && !(alive = ping(nullptr, &n)); i++) delay(300);
  }
  if (!alive) { logln("tag: not responding"); st_.valid = false; return false; }
  if (n != st_.expect_n) {  // tag rebooted (or first contact): its planes and screen are unknown
    st_.valid = false;
    if (!force_fast) fast = false;
  }
  const int CHUNK = 240;
  uint8_t buf[3 + CHUNK];
  const uint8_t *planes[2] = {c.black, c.red};
  int sent = 0;
  for (int pl = 0; pl < 2; pl++) {
    for (int off = 0; off < EpdCanvas::PLANE; off += CHUNK) {
      int len = min(CHUNK, EpdCanvas::PLANE - off);
      uint16_t crc = crc16(planes[pl] + off, len);
      if (st_.valid && st_.crc[pl][off / CHUNK] == crc) continue;
      buf[0] = pl; buf[1] = off; buf[2] = off >> 8;
      memcpy(buf + 3, planes[pl] + off, len);
      String r;
      for (int attempt = 0; attempt < 3; attempt++) {
        r = cmd('W', buf, len + 3, 500);
        if (r == "OK W") break;
      }
      if (r != "OK W") {
        logf("tag: write failed pl=%d off=%d: %s\n", pl, off, r.c_str());
        st_.valid = false;
        return false;
      }
      sent++;
    }
  }
  uint8_t dp[5] = {(uint8_t)(fast ? 1 : 0), FAST_RED_DRIVE, FAST_FRAMES, (uint8_t)led, (uint8_t)(led >> 8)};
  uint32_t t1 = millis();
  String r = cmd('D', dp, sizeof dp, wait ? 40000 : 0);
  logf("tag: %s refresh, %d chunks (%lu ms), refresh %lu ms: %s\n", fast ? "fast" : "full", sent,
                t1 - t0, millis() - t1, r.c_str());
  if (wait && !r.startsWith("OK D")) { st_.valid = false; return false; }
  for (int pl = 0; pl < 2; pl++)
    for (int off = 0; off < EpdCanvas::PLANE; off += CHUNK)
      st_.crc[pl][off / CHUNK] = crc16(planes[pl] + off, min(CHUNK, EpdCanvas::PLANE - off));
  st_.valid = true;
  st_.expect_n = n + 1;
  return true;
}

bool Tag::led(uint16_t level, uint16_t ms, bool red) {
  uint8_t p[5] = {(uint8_t)level, (uint8_t)(level >> 8), (uint8_t)ms, (uint8_t)(ms >> 8), (uint8_t)red};
  return cmd('L', p, 5, ms + 1000) == "OK L";
}

static uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t crc = ~0u;
  while (n--) {
    crc ^= *p++;
    for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc;
}

bool Tag::update_firmware(const uint8_t *fw, size_t n, String &msg, void (*progress)(int)) {
  if (n < 16 || memcmp(fw + 8, "KNLT", 4) != 0) { msg = "not a Telink firmware image"; return false; }
  if (!ping()) { msg = "tag not responding"; return false; }
  st_.valid = false;  // the tag reboots afterwards; resend everything
  st_.expect_n = -1;
  uint32_t crc = crc32(fw, n);
  uint8_t hdr[8] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24),
                    (uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24)};
  String r = cmd('O', hdr, 8, 15000);
  if (!r.startsWith("OK O")) { msg = "begin failed: " + r; return false; }
  uint8_t buf[4 + 256];
  for (size_t off = 0; off < n; off += 256) {
    size_t len = min((size_t)256, n - off);
    buf[0] = off; buf[1] = off >> 8; buf[2] = off >> 16; buf[3] = off >> 24;
    memcpy(buf + 4, fw + off, len);
    for (int attempt = 0; attempt < 3; attempt++) {
      r = cmd('B', buf, len + 4, 1000);
      if (r == "OK B") break;
    }
    if (r != "OK B") { msg = "write failed at " + String(off) + ": " + r; return false; }
    if (progress) progress(off * 100 / n);
  }
  r = cmd('E', nullptr, 0, 10000);
  if (r != "OK E") { msg = "verify failed: " + r; return false; }
  for (int i = 0; i < 20; i++) {  // wait for the tag to boot the new firmware
    delay(500);
    if (ping()) { msg = info_; if (progress) progress(100); return true; }
  }
  msg = "tag did not come back after the update";
  return false;
}
