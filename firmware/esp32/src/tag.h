#pragma once
// Driver for the Hanshow tag running tag_fw (hanshow_serial) over UART.
#include <Arduino.h>
#include <Adafruit_GFX.h>

enum { EPD_WHITE = 0, EPD_BLACK = 1, EPD_RED = 2 };

// 250x122 three-color canvas stored directly in the panel's native plane layout.
class EpdCanvas : public Adafruit_GFX {
public:
  static const int W = 250, H = 122, ROW = 16, PLANE = ROW * W;
  uint8_t black[PLANE];  // bit 0 = black
  uint8_t red[PLANE];    // bit 1 = red
  EpdCanvas() : Adafruit_GFX(W, H) { clear(); }
  void clear() { memset(black, 0xFF, PLANE); memset(red, 0, PLANE); }
  void drawPixel(int16_t x, int16_t y, uint16_t c) override {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    int i = (y >> 3) + (W - 1 - x) * ROW;
    uint8_t m = 0x80 >> (y & 7);
    if (c == EPD_BLACK) black[i] &= ~m; else black[i] |= m;
    if (c == EPD_RED) red[i] |= m; else red[i] &= ~m;
  }
  uint32_t hash() const;
  void rotate180() {  // turn the whole image upside down
    for (int y = 0; y < H / 2 + 1; y++)
      for (int x = 0; x < W; x++) {
        int x2 = W - 1 - x, y2 = H - 1 - y;
        if (y > y2 || (y == y2 && x >= x2)) continue;  // each pair once
        uint16_t a = get(x, y), b = get(x2, y2);
        drawPixel(x, y, b);
        drawPixel(x2, y2, a);
      }
  }
  uint16_t get(int x, int y) const {
    int i = (y >> 3) + (W - 1 - x) * ROW;
    uint8_t m = 0x80 >> (y & 7);
    return (red[i] & m) ? EPD_RED : (black[i] & m) ? EPD_WHITE : EPD_BLACK;
  }
};

// What the ESP32 knows about the tag's framebuffer; small enough to keep in RTC memory across
// deep sleep (per-chunk checksums instead of a copy of the planes).
struct TagState {
  static const int CHUNKS = (EpdCanvas::PLANE + 239) / 240;
  uint16_t crc[2][CHUNKS];
  bool valid;      // crc[] describes what the tag holds
  int expect_n;    // tag refresh count after our last refresh (a mismatch means it rebooted)
  uint16_t hard_resets;  // NRST resets because the tag didn't answer (diagnostics)
};

class Tag {
public:
  explicit Tag(HardwareSerial &s) : s_(s) { mtx_ = xSemaphoreCreateMutex(); }
  void begin(int rx, int tx) { s_.begin(115200, SERIAL_8N1, rx, tx); }
  bool ping(String *info = nullptr, int *refreshes = nullptr);
  // Upload the planes (only chunks that changed) and refresh:
  // full = flashing 3-color refresh (~13 s), fast = black/white-only update (~1 s).
  // led = brightness of the tag's LED while it refreshes, in 0.1 % units (0..1000).
  // force_fast: use the fast refresh even if the tag's screen content is unknown (after boot).
  // wait = false: return right after starting the refresh (the tag finishes on its own; the next
  // call checks its refresh count). Used before deep sleep.
  bool show(const EpdCanvas &c, bool fast, uint16_t led, bool force_fast = false, bool wait = true);
  TagState state() const { return st_; }
  void set_state(const TagState &s) { st_ = s; }
  // Light a tag LED without refreshing: level in 0.1 % units, red = red LED instead of green.
  bool led(uint16_t level, uint16_t ms, bool red = false);
  // Flash new tag firmware (a Telink .bin) over the UART link; the tag verifies the CRC,
  // switches flash banks and reboots. progress(0..100) is called along the way.
  bool update_firmware(const uint8_t *fw, size_t n, String &msg, void (*progress)(int));
  String info() const { return info_; }  // last ping reply, e.g. "OK P BWR213 v7 bank=0 n=3"
  void lock() { xSemaphoreTake(mtx_, portMAX_DELAY); }  // serialise worker vs. debug commands
  void unlock() { xSemaphoreGive(mtx_); }
private:
  String cmd(char c, const uint8_t *p, int n, uint32_t timeout_ms);
  HardwareSerial &s_;
  void wake();  // the tag sleeps when idle: a long low pulse on its RX wakes it
  TagState st_ = {{}, false, -1};
  uint32_t last_io_ = 0;  // millis() of the last exchange (0 = never: wake first)
  SemaphoreHandle_t mtx_;
  String info_;
};
