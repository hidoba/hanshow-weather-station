// Layout for the 250x122 black/white/red panel:
//   left  (0..103):   place, big icon + current temp, conditions, details
//   right top:        temperature curve, yesterday (thin) -> now (red) -> tomorrow (bold), rain chance bars
//   right bottom:     yesterday / today / next two days
#include "render.h"
#include <array>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/TomThumb.h>

static const char *WDAY[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const uint8_t DEG = 0xF8;  // degree sign in the built-in cp437 font

static const char *describe(int c) {
  switch (c) {
    case 0: return "Clear";
    case 1: return "Mainly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunder + hail";
  }
  return "?";
}

// ---------- icons (procedural, scale with s) ----------

static void sun(EpdCanvas &g, int cx, int cy, int r) {
  g.fillCircle(cx, cy, r, EPD_RED);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    int r1 = r + 2 + r / 4, r2 = r + 2 + r * 3 / 4;
    g.drawLine(cx + cosf(a) * r1, cy + sinf(a) * r1, cx + cosf(a) * r2, cy + sinf(a) * r2, EPD_RED);
    if (r >= 8) g.drawLine(cx + cosf(a) * r1 + 1, cy + sinf(a) * r1, cx + cosf(a) * r2 + 1, cy + sinf(a) * r2, EPD_RED);
  }
}

static void moon(EpdCanvas &g, int cx, int cy, int r) {
  g.fillCircle(cx, cy, r, EPD_BLACK);
  g.fillCircle(cx + r / 2, cy - r / 3, r * 3 / 4 + 1, EPD_WHITE);
}

static void cloud(EpdCanvas &g, int x, int y, int s) {
  struct { float x, y, r; } c[] = {{.28f, .62f, .18f}, {.52f, .46f, .26f}, {.76f, .62f, .18f}};
  int t = s >= 30 ? 2 : 1;  // outline thickness
  for (int pass = 0; pass < 2; pass++) {
    uint16_t col = pass == 0 ? EPD_BLACK : EPD_WHITE;
    int in = pass == 0 ? 0 : t;
    for (auto &k : c) g.fillCircle(x + k.x * s, y + k.y * s, k.r * s - in, col);
    g.fillRect(x + .28f * s, y + .62f * s - (.18f * s - in), .48f * s, 2 * (.18f * s - in) + 1, col);
  }
}

static bool is_white(EpdCanvas &g, int x, int y) {
  int i = (y >> 3) + (EpdCanvas::W - 1 - x) * EpdCanvas::ROW;
  return (g.black[i] & (0x80 >> (y & 7))) && !(g.red[i] & (0x80 >> (y & 7)));
}

static void hatch_cloud(EpdCanvas &g, int x, int y, int s) {
  // fill the cloud interior (white pixels enclosed by the outline) with a sparse pattern
  for (int yy = y + s * .25f; yy < y + s * .8f; yy++) {
    int first = -1, last = -1;
    for (int xx = x; xx < x + s; xx++)
      if (!is_white(g, xx, yy)) { if (first < 0) first = xx; last = xx; }
    for (int xx = first + 1; first >= 0 && xx < last; xx++)
      if (is_white(g, xx, yy) && ((xx + yy) % 3) == 0) g.drawPixel(xx, yy, EPD_BLACK);
  }
}

static void icon(EpdCanvas &g, int code, bool day, int x, int y, int s) {
  bool rain = (code >= 51 && code <= 67) || (code >= 80 && code <= 82);
  bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
  bool storm = code >= 95;
  if (code <= 1) {
    if (day) sun(g, x + s / 2, y + s / 2, s / 5);
    else moon(g, x + s / 2, y + s / 2, s / 3);
    return;
  }
  if (code == 45 || code == 48) {
    for (int i = 0; i < 4; i++) {
      int yy = y + s * (0.3f + i * 0.15f), off = (i & 1) * s / 8;
      g.fillRect(x + s / 8 + off, yy, s * 5 / 8, s >= 30 ? 2 : 1, EPD_BLACK);
    }
    return;
  }
  if (code == 2) {  // partly cloudy: sun peeking behind the cloud
    if (day) sun(g, x + s * .34f, y + s * .32f, s / 7);
    else moon(g, x + s * .34f, y + s * .3f, s / 5);
    cloud(g, x + s * .12f, y + s * .18f, s * .88f);
    return;
  }
  bool precip = rain || snow || storm;
  int cy = precip ? y - s / 8 : y;
  cloud(g, x, cy, s);
  if (code == 3) hatch_cloud(g, x, cy, s);
  int top = cy + s * .82f, n = 3;
  for (int i = 0; i < n; i++) {
    int px = x + s * (0.3f + i * 0.2f);
    if (rain) {
      bool heavy = code == 65 || code == 67 || code == 82;
      g.drawLine(px, top + 1, px - s / 10, top + s / 5, heavy ? EPD_RED : EPD_BLACK);
      if (s >= 30) g.drawLine(px + 1, top + 1, px + 1 - s / 10, top + s / 5, heavy ? EPD_RED : EPD_BLACK);
    } else if (snow) {
      int r = max(1, s / 16), yy = top + s / 8 + (i & 1) * s / 12;
      g.drawLine(px - r, yy, px + r, yy, EPD_BLACK);
      g.drawLine(px, yy - r, px, yy + r, EPD_BLACK);
      g.drawLine(px - r, yy - r, px + r, yy + r, EPD_BLACK);
      g.drawLine(px - r, yy + r, px + r, yy - r, EPD_BLACK);
    }
  }
  if (storm) {  // red lightning bolt
    int bx = x + s / 2, by = top - s / 10;
    g.fillTriangle(bx, by, bx - s / 6, by + s / 5, bx, by + s / 5, EPD_RED);
    g.fillTriangle(bx - s / 12, by + s / 6, bx + s / 12, by + s / 6, bx - s / 8, by + s * 2 / 5, EPD_RED);
  }
}

// ---------- text helpers ----------

// The GFX fonts only cover ASCII: decode UTF-8 and fold Latin accents to their base letter
// ("Zürich" -> "Zurich", "Kraków" -> "Krakow"); anything else becomes '?'.
static String ascii_fold(const String &in) {
  static const char LATIN1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";   // U+00C0..U+00FF
  static const char EXT_A[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";  // U+0100..U+017F
  String out;
  const uint8_t *p = (const uint8_t *)in.c_str();
  while (*p) {
    uint32_t cp;
    int n;
    if (*p < 0x80) { cp = *p; n = 1; }
    else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; n = 2; }
    else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; n = 3; }
    else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; n = 4; }
    else { p++; continue; }  // stray continuation byte
    int i = 1;
    for (; i < n && (p[i] & 0xC0) == 0x80; i++) cp = (cp << 6) | (p[i] & 0x3F);
    p += i;
    if (i < n) { out += '?'; continue; }  // truncated sequence
    if (cp < 0x80) out += (char)cp;
    else if (cp >= 0xC0 && cp <= 0xFF) out += LATIN1[cp - 0xC0];
    else if (cp >= 0x100 && cp <= 0x17F) out += EXT_A[cp - 0x100];
    else out += '?';
  }
  return out;
}

static void text(EpdCanvas &g, int x, int y, const String &s, uint16_t col, const GFXfont *f = nullptr) {
  g.setFont(f);
  g.setTextColor(col);
  g.setCursor(x, y);
  g.print(s);
}

static int text_w(EpdCanvas &g, const String &s, const GFXfont *f = nullptr) {
  int16_t x1, y1; uint16_t w, h;
  g.setFont(f);
  g.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

static void text_c(EpdCanvas &g, int cx, int y, const String &s, uint16_t col, const GFXfont *f = nullptr) {
  text(g, cx - text_w(g, s, f) / 2, y, s, col, f);
}

static String deg(float t) { return String((int)lroundf(t)) + (char)DEG; }

static void hdots(EpdCanvas &g, int x0, int x1, int y, int step = 2) {
  for (int x = x0; x <= x1; x += step) g.drawPixel(x, y, EPD_BLACK);
}

static void vdots(EpdCanvas &g, int x, int y0, int y1, uint16_t col = EPD_BLACK, int step = 2) {
  for (int y = y0; y <= y1; y += step) g.drawPixel(x, y, col);
}

// 11 px smiley: 0 happy, 1 neutral, 2 unhappy, 3 very unhappy
static void face(EpdCanvas &g, int cx, int cy, int mood, uint16_t col) {
  g.drawCircle(cx, cy, 5, col);
  g.drawPixel(cx - 2, cy - 2, col);
  g.drawPixel(cx + 2, cy - 2, col);
  static const int8_t mouth[4][7] = {  // y offset per x = -3..3, 9 = none
      {9, 1, 2, 2, 2, 1, 9},
      {9, 2, 2, 2, 2, 2, 9},
      {9, 2, 1, 1, 1, 2, 9},
      {3, 2, 1, 1, 1, 2, 3},
  };
  for (int i = 0; i < 7; i++)
    if (mouth[mood][i] != 9) g.drawPixel(cx - 3 + i, cy + mouth[mood][i], col);
}

// ---------- sections ----------

static void draw_current(EpdCanvas &g, const Weather &w, const String &place) {
  g.cp437(true);
  g.setTextSize(1);
  String name = ascii_fold(place);
  text(g, 2, 2, name.length() > 16 ? name.substring(0, 16) : name, EPD_BLACK);

  icon(g, w.code, w.is_day, 0, 12, 42);

  // big temperature, shrink the font if it doesn't fit
  String t = String((int)lroundf(w.temp));
  const GFXfont *f = &FreeSansBold18pt7b;
  if (text_w(g, t, f) > 48) f = &FreeSansBold12pt7b;
  int tw = text_w(g, t, f);
  int tx = 44 + (52 - tw - 7) / 2;
  text(g, tx, 45, t, EPD_BLACK, f);
  g.drawCircle(tx + tw + 5, 23, 3, EPD_RED);
  g.drawCircle(tx + tw + 5, 23, 2, EPD_RED);

  text(g, 2, 58, describe(w.code), EPD_BLACK);
  text(g, 2, 69, "Feels " + deg(w.feels), EPD_BLACK);
  text(g, 2, 80, String(w.humidity) + "%  " + String((int)lroundf(w.wind)) + (w.fahrenheit ? "mph" : "km/h"), EPD_BLACK);

  if (!isnan(w.pm25)) {
    int v = lroundf(w.pm25);  // judge the number that is shown
    String pm = "PM2.5 " + String(v);
    int mood = v < 5 ? 0 : v < 15 ? 1 : v <= 25 ? 2 : 3;
    uint16_t col = mood == 3 ? EPD_RED : EPD_BLACK;
    text(g, 2, 91, pm, col);
    face(g, 2 + text_w(g, pm) + 8, 94, mood, col);
  }
}

static void draw_clock(EpdCanvas &g, const Clock &c) {
  char upd[16], now[8];
  snprintf(upd, sizeof upd, "%supd %02d:%02d", c.stale ? "! " : "", c.upd_hour, c.upd_min);
  text(g, 101 - text_w(g, upd, &TomThumb), 119, upd, EPD_BLACK, &TomThumb);  // right-aligned to the divider
  if (c.show) {
    snprintf(now, sizeof now, "%02d:%02d", c.hour, c.minute);
    text(g, 1, 119, now, EPD_BLACK, &FreeSansBold12pt7b);
  }
}

static void draw_curve(EpdCanvas &g, const Weather &w) {
  const int X0 = 118, X1 = 247, Y0 = 6, Y1 = 50, N = Weather::CURVE;
  float lo = 1e9, hi = -1e9;
  for (int i = 0; i < N; i++)
    if (!isnan(w.curve[i])) { lo = min(lo, w.curve[i]); hi = max(hi, w.curve[i]); }
  if (hi < lo) return;
  float span = max(hi - lo, w.fahrenheit ? 7.0f : 4.0f), mid = (hi + lo) / 2;
  lo = mid - span / 2; hi = mid + span / 2;
  auto px = [&](int i) { return X0 + (X1 - X0) * i / (N - 1); };
  auto py = [&](float t) { return (int)lroundf(Y1 - (Y1 - Y0) * (t - lo) / (hi - lo)); };

  // axis labels and frame
  g.setFont(&TomThumb);
  text(g, 106, Y0 + 5, String((int)lroundf(hi)), EPD_BLACK, &TomThumb);
  text(g, 106, Y1 + 1, String((int)lroundf(lo)), EPD_BLACK, &TomThumb);
  hdots(g, X0, X1, Y1 + 1, 2);
  if (lo < 0 && hi > 0) hdots(g, X0, X1, py(0), 4);  // freezing line

  // rain probability: dithered bars under the curve
  for (int i = 0; i < N - 1; i++) {
    int h = (Y1 - Y0) * w.precip[i] / 100 / 2;
    for (int x = px(i); x < px(i + 1); x++)
      for (int y = Y1 - h; y <= Y1; y++)
        if (((x + y) & 1) == 0) g.drawPixel(x, y, EPD_BLACK);
  }

  // midnights, and a label centred on each day's visible stretch
  static const char *DAYLBL[] = {"yday", "today", "tmr"};
  int seg_start = 0;
  for (int i = 1; i <= N; i++) {
    bool midnight = i < N && (w.curve_hour0 + i) % 24 == 0;
    if (midnight) vdots(g, px(i), Y0, Y1, EPD_BLACK, 3);
    if (midnight || i == N) {
      int day = (w.curve_hour0 + seg_start) / 24;  // 0 = yesterday
      if (i - seg_start >= 7 && day < 3) text_c(g, (px(seg_start) + px(i - 1)) / 2, Y1 + 8, DAYLBL[day], EPD_BLACK, &TomThumb);
      seg_start = i;
    }
  }

  // past: thick line; future: thin line
  for (int i = 0; i < N - 1; i++) {
    if (isnan(w.curve[i]) || isnan(w.curve[i + 1])) continue;
    int x0 = px(i), y0 = py(w.curve[i]), x1 = px(i + 1), y1 = py(w.curve[i + 1]);
    g.drawLine(x0, y0, x1, y1, EPD_BLACK);
    if (i < 24) { g.drawLine(x0, y0 - 1, x1, y1 - 1, EPD_BLACK); g.drawLine(x0, y0 + 1, x1, y1 + 1, EPD_BLACK); }
  }

  // now marker
  int nx = px(24) + (px(25) - px(24)) * w.minute / 60, ny = py(w.temp);
  vdots(g, nx, Y0 - 2, Y1, EPD_RED, 2);
  g.fillCircle(nx, ny, 3, EPD_RED);

  // label the upcoming peak in red
  int im = 24;
  for (int i = 25; i < N; i++) if (w.curve[i] > w.curve[im]) im = i;
  if (im > 26) {
    int x = px(im), y = py(w.curve[im]);
    g.fillCircle(x, y, 2, EPD_RED);
    String s = deg(w.curve[im]);
    g.cp437(true);
    int sw = text_w(g, s);
    if (y - 10 >= 0) {  // above the peak
      text(g, min(max(x - sw / 2, X0), X1 - sw), y - 10, s, EPD_RED);
    } else {            // no room above: beside the dot
      int lx = x + 5 + sw <= X1 ? x + 5 : x - 5 - sw;
      text(g, lx, max(y - 3, 0), s, EPD_RED);
    }
  }
}


// ---------- holiday illustrations (replace the temperature curve) ----------
// Drawn on a 56x56 grid, scaled to the box.

namespace {
struct Grid {
  int x, y;
  float k;
  int X(float v) const { return x + lroundf(v * k); }
  int Y(float v) const { return y + lroundf(v * k); }
  int R(float v) const { return max(1, (int)lroundf(v * k)); }
};

void star(EpdCanvas &g, int cx, int cy, int r, uint16_t col) {
  for (int i = 0; i < 5; i++) {
    float a0 = -PI / 2 + i * 2 * PI / 5, a1 = a0 + PI / 5, a2 = a0 + 2 * PI / 5;
    int ox = cx + cosf(a0) * r, oy = cy + sinf(a0) * r;
    int ix = cx + cosf(a1) * r * .42f, iy = cy + sinf(a1) * r * .42f;
    int px = cx + cosf(a2) * r, py = cy + sinf(a2) * r;
    g.fillTriangle(cx, cy, ox, oy, ix, iy, col);
    g.fillTriangle(cx, cy, ix, iy, px, py, col);
  }
}

// Union of circles with a black outline: fill black at r+1 first, then the colour at r.
void blob(EpdCanvas &g, const Grid &q, std::initializer_list<std::array<float, 3>> circles, uint16_t col) {
  for (auto &c : circles) g.fillCircle(q.X(c[0]), q.Y(c[1]), q.R(c[2]) + 1, EPD_BLACK);
  for (auto &c : circles) g.fillCircle(q.X(c[0]), q.Y(c[1]), q.R(c[2]), col);
}

void burst(EpdCanvas &g, int cx, int cy, int r, uint16_t col) {
  for (int i = 0; i < 12; i++) {
    float a = i * PI / 6;
    g.drawLine(cx + cosf(a) * r * .35f, cy + sinf(a) * r * .35f, cx + cosf(a) * r * .8f, cy + sinf(a) * r * .8f, col);
    g.fillCircle(cx + cosf(a) * r, cy + sinf(a) * r, 1, col);
  }
}

void pumpkin(EpdCanvas &g, const Grid &q) {
  g.fillRect(q.X(26), q.Y(12), q.R(5), q.R(9), EPD_BLACK);                        // stem
  blob(g, q, {{{19, 35, 14}}, {{37, 35, 14}}, {{28, 35, 16}}}, EPD_RED);            // body
  g.drawLine(q.X(22), q.Y(22), q.X(21), q.Y(48), EPD_BLACK);                      // ribs
  g.drawLine(q.X(34), q.Y(22), q.X(35), q.Y(48), EPD_BLACK);
  g.fillTriangle(q.X(16), q.Y(32), q.X(24), q.Y(32), q.X(20), q.Y(25), EPD_WHITE);  // eyes
  g.fillTriangle(q.X(32), q.Y(32), q.X(40), q.Y(32), q.X(36), q.Y(25), EPD_WHITE);
  g.fillTriangle(q.X(26), q.Y(38), q.X(30), q.Y(38), q.X(28), q.Y(34), EPD_WHITE);  // nose
  g.fillRoundRect(q.X(15), q.Y(41), q.R(26), q.R(6), 2, EPD_WHITE);                // grin
  g.fillRect(q.X(20), q.Y(41), q.R(3), q.R(3), EPD_RED);                           // teeth
  g.fillRect(q.X(33), q.Y(44), q.R(3), q.R(3), EPD_RED);
  int bx = q.X(47), by = q.Y(8);                                                  // bat
  g.fillCircle(bx, by, q.R(2), EPD_BLACK);
  g.fillTriangle(bx, by, bx - q.R(9), by - q.R(4), bx - q.R(5), by + q.R(3), EPD_BLACK);
  g.fillTriangle(bx, by, bx + q.R(9), by - q.R(4), bx + q.R(5), by + q.R(3), EPD_BLACK);
}

void xmas_tree(EpdCanvas &g, const Grid &q) {
  g.fillTriangle(q.X(28), q.Y(8), q.X(17), q.Y(22), q.X(39), q.Y(22), EPD_BLACK);
  g.fillTriangle(q.X(28), q.Y(15), q.X(12), q.Y(33), q.X(44), q.Y(33), EPD_BLACK);
  g.fillTriangle(q.X(28), q.Y(23), q.X(7), q.Y(45), q.X(49), q.Y(45), EPD_BLACK);
  g.fillRect(q.X(24), q.Y(45), q.R(8), q.R(7), EPD_BLACK);                           // trunk
  for (auto &o : {std::array<int, 2>{24, 19}, {33, 26}, {20, 31}, {30, 36}, {40, 41}, {16, 41}, {27, 43}})
    g.fillCircle(q.X(o[0]), q.Y(o[1]), q.R(2), EPD_RED);                             // baubles
  star(g, q.X(28), q.Y(7), q.R(7), EPD_RED);
  g.fillRect(q.X(40), q.Y(46), q.R(12), q.R(9), EPD_RED);                           // present
  g.fillRect(q.X(45), q.Y(46), q.R(2), q.R(9), EPD_WHITE);
  g.fillRect(q.X(40), q.Y(49), q.R(12), q.R(2), EPD_WHITE);
}

void fireworks(EpdCanvas &g, const Grid &q) {
  burst(g, q.X(20), q.Y(20), q.R(18), EPD_RED);
  burst(g, q.X(43), q.Y(31), q.R(12), EPD_BLACK);
  burst(g, q.X(15), q.Y(46), q.R(8), EPD_RED);
  for (int yy = q.Y(44); yy < q.Y(56); yy += 2) g.drawPixel(q.X(43), yy, EPD_BLACK);  // rocket trail
  for (auto &p : {std::array<int, 2>{50, 8}, {34, 50}, {6, 30}}) {                     // sparkles
    g.drawFastHLine(q.X(p[0]) - 2, q.Y(p[1]), 5, EPD_BLACK);
    g.drawFastVLine(q.X(p[0]), q.Y(p[1]) - 2, 5, EPD_BLACK);
  }
}

void lantern(EpdCanvas &g, const Grid &q) {
  g.drawLine(q.X(28), q.Y(0), q.X(28), q.Y(7), EPD_BLACK);                         // string
  g.fillRect(q.X(19), q.Y(6), q.R(18), q.R(5), EPD_BLACK);                         // cap
  g.fillRoundRect(q.X(7) - 1, q.Y(10) - 1, q.R(42) + 2, q.R(31) + 2, q.R(14), EPD_BLACK);
  g.fillRoundRect(q.X(7), q.Y(10), q.R(42), q.R(31), q.R(14), EPD_RED);            // body
  for (int rx : {17, 28, 39}) g.drawLine(q.X(rx), q.Y(12), q.X(rx), q.Y(39), EPD_BLACK);  // ribs
  g.fillRect(q.X(19), q.Y(40), q.R(18), q.R(5), EPD_BLACK);                        // bottom cap
  g.fillCircle(q.X(28), q.Y(47), q.R(2), EPD_BLACK);                               // knot
  for (int dx = -5; dx <= 5; dx += 2) g.drawLine(q.X(28), q.Y(48), q.X(28 + dx), q.Y(56), EPD_RED);  // tassel
}

void goldfish_bowl(EpdCanvas &g, const Grid &q) {
  int cx = q.X(28), cy = q.Y(33), r = q.R(21);
  g.drawCircle(cx, cy, r, EPD_BLACK);
  g.drawCircle(cx, cy, r - 1, EPD_BLACK);
  g.fillRect(q.X(15), q.Y(9), q.R(26), q.R(8), EPD_WHITE);                         // open top
  g.fillRect(q.X(13), q.Y(16), q.R(30), 2, EPD_BLACK);                             // rim
  for (int xx = q.X(9); xx < q.X(48); xx += 3) g.drawPixel(xx, q.Y(22), EPD_BLACK);  // water line
  for (int yy = q.Y(47); yy < cy + r - 1; yy++)                                    // gravel
    for (int xx = cx - r; xx < cx + r; xx++)
      if ((xx - cx) * (xx - cx) + (yy - cy) * (yy - cy) < (r - 2) * (r - 2) && ((xx * 7 + yy * 3) % 5) == 0)
        g.drawPixel(xx, yy, EPD_BLACK);
  g.fillRoundRect(q.X(15), q.Y(31), q.R(18), q.R(10), q.R(5), EPD_RED);            // goldfish
  g.fillTriangle(q.X(31), q.Y(36), q.X(40), q.Y(29), q.X(40), q.Y(43), EPD_RED);
  g.fillRect(q.X(19), q.Y(34), 2, 2, EPD_BLACK);                                   // eye
  g.drawCircle(q.X(13), q.Y(27), q.R(1.5f), EPD_BLACK);                            // bubbles
  g.drawCircle(q.X(10), q.Y(24), 1, EPD_BLACK);
}

void crescent(EpdCanvas &g, const Grid &q) {
  g.fillCircle(q.X(24), q.Y(29), q.R(19), EPD_BLACK);
  g.fillCircle(q.X(32), q.Y(23), q.R(16), EPD_WHITE);
  star(g, q.X(39), q.Y(31), q.R(9), EPD_RED);
  star(g, q.X(48), q.Y(8), q.R(4), EPD_BLACK);
  star(g, q.X(9), q.Y(6), q.R(3), EPD_BLACK);
}

void apple_honey(EpdCanvas &g, const Grid &q) {
  g.drawLine(q.X(20), q.Y(25), q.X(22), q.Y(15), EPD_BLACK);                      // stem
  g.drawLine(q.X(21), q.Y(25), q.X(23), q.Y(15), EPD_BLACK);
  g.fillTriangle(q.X(22), q.Y(18), q.X(31), q.Y(12), q.X(28), q.Y(21), EPD_BLACK);  // leaf
  blob(g, q, {{{15, 36, 12}}, {{25, 36, 12}}, {{20, 41, 11}}}, EPD_RED);             // apple
  g.fillCircle(q.X(14), q.Y(32), q.R(2), EPD_WHITE);                              // shine
  g.fillRoundRect(q.X(39), q.Y(33), q.R(16), q.R(21), q.R(4), EPD_BLACK);          // honey pot
  g.fillRect(q.X(38), q.Y(29), q.R(18), q.R(5), EPD_BLACK);                        // lid
  g.fillRect(q.X(42), q.Y(39), q.R(10), q.R(8), EPD_WHITE);                        // label
  g.drawLine(q.X(47), q.Y(29), q.X(54), q.Y(13), EPD_BLACK);                      // dipper
  g.fillCircle(q.X(54), q.Y(12), q.R(3), EPD_BLACK);
}

void heart(EpdCanvas &g, int cx, int cy, int r, uint16_t col) {  // r = lobe radius
  g.fillCircle(cx - r, cy, r, col);
  g.fillCircle(cx + r, cy, r, col);
  g.fillTriangle(cx - 2 * r, cy + r / 3, cx + 2 * r, cy + r / 3, cx, cy + 2 * r + r / 2, col);
}

void cupid_heart(EpdCanvas &g, const Grid &q) {
  g.drawLine(q.X(3), q.Y(51), q.X(52), q.Y(9), EPD_BLACK);                        // arrow shaft
  g.drawLine(q.X(4), q.Y(52), q.X(53), q.Y(10), EPD_BLACK);
  g.fillTriangle(q.X(54), q.Y(5), q.X(46), q.Y(9), q.X(52), q.Y(15), EPD_BLACK);  // arrow head
  g.drawLine(q.X(3), q.Y(51), q.X(3), q.Y(44), EPD_BLACK);                        // fletching
  g.drawLine(q.X(3), q.Y(51), q.X(10), q.Y(51), EPD_BLACK);
  g.drawLine(q.X(7), q.Y(47), q.X(7), q.Y(41), EPD_BLACK);
  g.drawLine(q.X(7), q.Y(47), q.X(13), q.Y(47), EPD_BLACK);
  heart(g, q.X(27), q.Y(23), q.R(10) + 1, EPD_BLACK);                             // outline
  heart(g, q.X(27), q.Y(23), q.R(10), EPD_RED);
  g.fillCircle(q.X(21), q.Y(20), q.R(2), EPD_WHITE);                              // shine
  heart(g, q.X(47), q.Y(38), q.R(3), EPD_RED);                                    // little hearts
  heart(g, q.X(9), q.Y(10), q.R(2), EPD_RED);
}

void water_gun(EpdCanvas &g, const Grid &q) {
  g.fillRoundRect(q.X(4), q.Y(30), q.R(26), q.R(10), 3, EPD_BLACK);               // body
  g.fillRect(q.X(29), q.Y(32), q.R(10), q.R(5), EPD_BLACK);                        // barrel
  g.fillRoundRect(q.X(8) - 1, q.Y(18) - 1, q.R(15) + 2, q.R(13) + 2, 4, EPD_BLACK);
  g.fillRoundRect(q.X(8), q.Y(18), q.R(15), q.R(13), 4, EPD_RED);                  // tank
  g.fillTriangle(q.X(9), q.Y(39), q.X(19), q.Y(39), q.X(14), q.Y(55), EPD_BLACK);  // grip
  g.fillTriangle(q.X(9), q.Y(39), q.X(14), q.Y(55), q.X(5), q.Y(55), EPD_BLACK);
  g.drawLine(q.X(22), q.Y(40), q.X(21), q.Y(46), EPD_BLACK);                      // trigger
  for (int i = 0; i < 6; i++) {                                                   // spray
    float t = i / 5.0f;
    int xx = q.X(41 + 13 * t), yy = q.Y(33 - 26 * t * t);
    g.drawCircle(xx, yy, q.R(1.5f + t * 2), EPD_BLACK);
  }
  g.fillCircle(q.X(52), q.Y(6), q.R(2), EPD_RED);                                  // splash
  g.fillCircle(q.X(46), q.Y(4), q.R(1), EPD_RED);
}

void easter_egg(EpdCanvas &g, const Grid &q) {
  for (int dx : {-6, 6}) {  // bunny ears peeking out behind the egg
    g.fillRoundRect(q.X(28 + dx - 4), q.Y(1), q.R(8), q.R(26), q.R(4), EPD_BLACK);
    g.fillRoundRect(q.X(28 + dx - 2), q.Y(4), q.R(4), q.R(16), q.R(2), EPD_RED);
  }
  blob(g, q, {{{28, 38, 15}}, {{28, 30, 12}}, {{28, 24, 9}}}, EPD_WHITE);
  for (int i = 0; i < 6; i++) {  // red zigzag band
    int x0 = q.X(14 + i * 5), x1 = q.X(16.5f + i * 5), x2 = q.X(19 + i * 5);
    g.fillTriangle(x0, q.Y(36), x1, q.Y(30), x2, q.Y(36), EPD_RED);
  }
  g.fillRect(q.X(14), q.Y(36), q.R(29), q.R(3), EPD_RED);
  for (auto &p : {std::array<int, 2>{22, 45}, {28, 48}, {34, 45}, {28, 20}}) g.fillCircle(q.X(p[0]), q.Y(p[1]), q.R(1.5f), EPD_BLACK);
}

void flame(EpdCanvas &g, int cx, int cy, int r) {  // red teardrop with a bright core
  g.fillCircle(cx, cy, r, EPD_RED);
  g.fillTriangle(cx - r, cy, cx + r, cy, cx, cy - r * 3, EPD_RED);
  if (r >= 3) g.fillCircle(cx, cy, r / 2, EPD_WHITE);
}

void diya(EpdCanvas &g, const Grid &q) {
  g.fillCircle(q.X(26), q.Y(34), q.R(17), EPD_BLACK);  // clay bowl
  g.fillRect(q.X(8), q.Y(15), q.R(40), q.R(19), EPD_WHITE);
  g.fillTriangle(q.X(40), q.Y(34), q.X(52), q.Y(29), q.X(41), q.Y(40), EPD_BLACK);  // spout
  g.fillRect(q.X(10), q.Y(34), q.R(32), q.R(2), EPD_BLACK);
  for (int i = 0; i < 4; i++) g.fillCircle(q.X(15 + i * 7), q.Y(42), q.R(1.5f), EPD_WHITE);  // pattern
  flame(g, q.X(47), q.Y(24), q.R(4));
  for (auto &p : {std::array<int, 2>{12, 12}, {30, 8}, {22, 22}, {52, 8}}) {  // sparkles
    g.drawFastHLine(q.X(p[0]) - 2, q.Y(p[1]), 5, EPD_RED);
    g.drawFastVLine(q.X(p[0]), q.Y(p[1]) - 2, 5, EPD_RED);
  }
}

void menorah(EpdCanvas &g, const Grid &q, int night) {
  g.fillRect(q.X(18), q.Y(51), q.R(20), q.R(4), EPD_BLACK);  // base
  g.fillRect(q.X(26), q.Y(30), q.R(4), q.R(22), EPD_BLACK);  // stem
  g.fillRect(q.X(4), q.Y(30), q.R(48), q.R(3), EPD_BLACK);   // arms
  for (int i = 0; i < 9; i++) {
    bool shamash = i == 4;
    int cx = q.X(6 + i * 5.5f), top = q.Y(shamash ? 14 : 20);
    g.fillRect(cx - 1, top, 3, q.Y(30) - top, EPD_BLACK);  // candle
    int slot = i < 4 ? i : i - 1;                          // candles lit from the right
    bool lit = shamash || (i != 4 && 7 - slot < night);
    if (lit) flame(g, cx, top - q.R(3), q.R(2));
  }
}

void eid_lantern(EpdCanvas &g, const Grid &q) {
  g.drawCircle(q.X(22), q.Y(4), q.R(3), EPD_BLACK);  // ring
  g.fillTriangle(q.X(11), q.Y(16), q.X(33), q.Y(16), q.X(22), q.Y(6), EPD_BLACK);  // roof
  g.fillRect(q.X(11), q.Y(16), q.R(23), q.R(28), EPD_BLACK);  // frame
  g.fillRect(q.X(14), q.Y(19), q.R(7), q.R(22), EPD_RED);     // glass panes
  g.fillRect(q.X(24), q.Y(19), q.R(7), q.R(22), EPD_RED);
  flame(g, q.X(22), q.Y(33), q.R(2));
  g.fillTriangle(q.X(11), q.Y(44), q.X(33), q.Y(44), q.X(22), q.Y(52), EPD_BLACK);  // base
  g.fillCircle(q.X(45), q.Y(18), q.R(10), EPD_BLACK);  // crescent
  g.fillCircle(q.X(49), q.Y(15), q.R(9), EPD_WHITE);
  star(g, q.X(50), q.Y(36), q.R(4), EPD_RED);
}

void sheep(EpdCanvas &g, const Grid &q) {
  for (int lx : {16, 22, 34, 40}) g.fillRect(q.X(lx), q.Y(38), q.R(3), q.R(14), EPD_BLACK);  // legs
  blob(g, q, {{{16, 28, 9}}, {{26, 24, 10}}, {{37, 28, 9}}, {{21, 35, 8}}, {{33, 35, 8}}}, EPD_WHITE);  // fleece
  g.fillRoundRect(q.X(42), q.Y(20), q.R(12), q.R(15), q.R(5), EPD_BLACK);  // head
  g.fillTriangle(q.X(43), q.Y(22), q.X(38), q.Y(26), q.X(44), q.Y(27), EPD_BLACK);  // ear
  g.fillCircle(q.X(49), q.Y(25), 1, EPD_WHITE);  // eye
  g.fillRect(q.X(50), q.Y(31), q.R(3), q.R(2), EPD_RED);  // tongue
}

void holi(EpdCanvas &g, const Grid &q) {
  auto splat = [&](float cx, float cy, float r, uint16_t col, unsigned seed) {
    g.fillCircle(q.X(cx), q.Y(cy), q.R(r), col);
    for (int i = 0; i < 9; i++) {
      seed = seed * 1103515245 + 12345;
      float a = i * 0.7f + (seed >> 16) % 100 / 100.0f, d = r * (0.9f + (seed >> 8) % 60 / 100.0f);
      g.fillCircle(q.X(cx + cosf(a) * d), q.Y(cy + sinf(a) * d), q.R(r * (0.18f + (seed % 30) / 100.0f)), col);
      g.fillCircle(q.X(cx + cosf(a) * d * 1.6f), q.Y(cy + sinf(a) * d * 1.6f), q.R(1), col);  // droplets
    }
  };
  splat(20, 22, 11, EPD_RED, 7);
  splat(40, 38, 8, EPD_BLACK, 3);
  splat(14, 46, 4, EPD_RED, 11);
  splat(46, 12, 4, EPD_BLACK, 5);
}

void moon_cake(EpdCanvas &g, const Grid &q) {
  g.fillCircle(q.X(22), q.Y(20), q.R(18), EPD_BLACK);  // full moon
  g.fillCircle(q.X(22), q.Y(20), q.R(16), EPD_WHITE);
  for (auto &c : {std::array<float, 3>{16, 14, 3}, {27, 24, 4}, {15, 26, 2}}) g.drawCircle(q.X(c[0]), q.Y(c[1]), q.R(c[2]), EPD_BLACK);
  for (int i = 0; i < 12; i++) {  // mooncake with scalloped edge
    float a = i * PI / 6;
    g.fillCircle(q.X(38 + cosf(a) * 12), q.Y(40 + sinf(a) * 12), q.R(3), EPD_RED);
  }
  g.fillCircle(q.X(38), q.Y(40), q.R(12), EPD_RED);
  g.drawRect(q.X(33), q.Y(35), q.R(11), q.R(11), EPD_WHITE);  // pattern
  g.drawLine(q.X(33), q.Y(35), q.X(43), q.Y(45), EPD_WHITE);
  g.drawLine(q.X(43), q.Y(35), q.X(33), q.Y(45), EPD_WHITE);
}

void turkey(EpdCanvas &g, const Grid &q) {
  for (int i = 0; i < 7; i++) {  // tail fan
    float a = PI + i * PI / 6;
    g.fillCircle(q.X(26 + cosf(a) * 17), q.Y(30 + sinf(a) * 17), q.R(7), i % 2 ? EPD_RED : EPD_BLACK);
  }
  blob(g, q, {{{26, 34, 12}}}, EPD_BLACK);                        // body
  g.fillCircle(q.X(40), q.Y(26), q.R(6), EPD_BLACK);              // head
  g.fillCircle(q.X(42), q.Y(24), 1, EPD_WHITE);                   // eye
  g.fillTriangle(q.X(45), q.Y(25), q.X(51), q.Y(27), q.X(45), q.Y(29), EPD_BLACK);  // beak
  g.fillRoundRect(q.X(43), q.Y(29), q.R(3), q.R(8), 1, EPD_RED);  // wattle
  g.drawLine(q.X(22), q.Y(46), q.X(20), q.Y(54), EPD_BLACK);      // feet
  g.drawLine(q.X(30), q.Y(46), q.X(32), q.Y(54), EPD_BLACK);
}

void shamrock(EpdCanvas &g, const Grid &q) {
  for (int i = 0; i < 3; i++) {  // three heart-shaped leaves
    float a = -PI / 2 + i * 2 * PI / 3;
    float cx = 28 + cosf(a) * 11, cy = 24 + sinf(a) * 11;
    g.fillCircle(q.X(cx + cosf(a + 1.3f) * 5), q.Y(cy + sinf(a + 1.3f) * 5), q.R(8), EPD_BLACK);
    g.fillCircle(q.X(cx + cosf(a - 1.3f) * 5), q.Y(cy + sinf(a - 1.3f) * 5), q.R(8), EPD_BLACK);
    g.fillTriangle(q.X(28), q.Y(24), q.X(cx + cosf(a + 1.3f) * 9), q.Y(cy + sinf(a + 1.3f) * 9),
                   q.X(cx + cosf(a - 1.3f) * 9), q.Y(cy + sinf(a - 1.3f) * 9), EPD_BLACK);
  }
  g.drawLine(q.X(28), q.Y(30), q.X(34), q.Y(54), EPD_BLACK);  // stem
  g.drawLine(q.X(29), q.Y(30), q.X(35), q.Y(54), EPD_BLACK);
  g.fillCircle(q.X(28), q.Y(24), q.R(3), EPD_RED);
}

void venus(EpdCanvas &g, const Grid &q) {
  g.fillCircle(q.X(20), q.Y(18), q.R(14), EPD_RED);  // the symbol
  g.fillCircle(q.X(20), q.Y(18), q.R(9), EPD_WHITE);
  g.fillRect(q.X(17), q.Y(31), q.R(6), q.R(22), EPD_RED);
  g.fillRect(q.X(10), q.Y(40), q.R(20), q.R(6), EPD_RED);
  g.drawLine(q.X(44), q.Y(54), q.X(44), q.Y(30), EPD_BLACK);  // tulip
  g.fillTriangle(q.X(44), q.Y(46), q.X(52), q.Y(38), q.X(46), q.Y(50), EPD_BLACK);
  g.fillCircle(q.X(44), q.Y(24), q.R(6), EPD_BLACK);
  g.fillTriangle(q.X(38), q.Y(24), q.X(40), q.Y(14), q.X(44), q.Y(22), EPD_BLACK);
  g.fillTriangle(q.X(50), q.Y(24), q.X(48), q.Y(14), q.X(44), q.Y(22), EPD_BLACK);
  g.fillTriangle(q.X(41), q.Y(22), q.X(44), q.Y(13), q.X(47), q.Y(22), EPD_BLACK);
}

void jester(EpdCanvas &g, const Grid &q) {
  g.fillTriangle(q.X(8), q.Y(40), q.X(24), q.Y(40), q.X(4), q.Y(12), EPD_RED);      // left point
  g.fillTriangle(q.X(20), q.Y(40), q.X(36), q.Y(40), q.X(28), q.Y(6), EPD_BLACK);   // middle
  g.fillTriangle(q.X(32), q.Y(40), q.X(48), q.Y(40), q.X(52), q.Y(12), EPD_RED);    // right
  for (auto &p : {std::array<int, 2>{4, 10}, {28, 4}, {52, 10}}) {                 // bells
    g.fillCircle(q.X(p[0]), q.Y(p[1]), q.R(3.5f), EPD_BLACK);
    g.fillCircle(q.X(p[0]), q.Y(p[1]), q.R(2), EPD_WHITE);
  }
  g.fillRoundRect(q.X(6), q.Y(40), q.R(44), q.R(8), q.R(3), EPD_BLACK);  // band
  for (int i = 0; i < 4; i++) g.fillCircle(q.X(12 + i * 11), q.Y(44), q.R(1.5f), EPD_RED);
}

void pi_pie(EpdCanvas &g, const Grid &q) {
  g.fillRoundRect(q.X(2), q.Y(6), q.R(34), q.R(5), 2, EPD_BLACK);  // the pi symbol
  g.fillRect(q.X(9), q.Y(10), q.R(5), q.R(30), EPD_BLACK);
  g.fillRect(q.X(23), q.Y(10), q.R(5), q.R(26), EPD_BLACK);
  g.fillCircle(q.X(28), q.Y(38), q.R(4), EPD_BLACK);
  g.fillCircle(q.X(30), q.Y(36), q.R(2), EPD_WHITE);
  g.fillCircle(q.X(42), q.Y(42), q.R(12), EPD_RED);  // pie with a slice missing
  g.drawCircle(q.X(42), q.Y(42), q.R(12), EPD_BLACK);
  g.fillTriangle(q.X(42), q.Y(42), q.X(56), q.Y(34), q.X(56), q.Y(50), EPD_WHITE);
  g.drawLine(q.X(42), q.Y(42), q.X(53), q.Y(36), EPD_BLACK);
  g.drawLine(q.X(42), q.Y(42), q.X(53), q.Y(48), EPD_BLACK);
}

void cool_sun(EpdCanvas &g, const Grid &q) {
  sun(g, q.X(28), q.Y(28), q.R(14));
  g.fillRoundRect(q.X(15), q.Y(22), q.R(11), q.R(8), q.R(3), EPD_BLACK);  // sunglasses
  g.fillRoundRect(q.X(30), q.Y(22), q.R(11), q.R(8), q.R(3), EPD_BLACK);
  g.fillRect(q.X(25), q.Y(23), q.R(6), 2, EPD_BLACK);
  g.drawPixel(q.X(18), q.Y(24), EPD_WHITE);
  g.drawPixel(q.X(33), q.Y(24), EPD_WHITE);
  for (int x = -5; x <= 5; x++) g.drawPixel(q.X(28 + x), q.Y(37) - x * x / 8, EPD_WHITE);  // smile (ends up)
}

void snowflake(EpdCanvas &g, const Grid &q) {
  int cx = q.X(26), cy = q.Y(28), r = q.R(22);
  for (int i = 0; i < 6; i++) {
    float a = i * PI / 3;
    int ex = cx + cosf(a) * r, ey = cy + sinf(a) * r;
    g.drawLine(cx, cy, ex, ey, EPD_BLACK);
    g.drawLine(cx + 1, cy, ex + 1, ey, EPD_BLACK);
    for (float t : {0.45f, 0.72f}) {  // branches
      int bx = cx + cosf(a) * r * t, by = cy + sinf(a) * r * t, bl = r * (1.05f - t) * 0.6f;
      g.drawLine(bx, by, bx + cosf(a + 0.8f) * bl, by + sinf(a + 0.8f) * bl, EPD_BLACK);
      g.drawLine(bx, by, bx + cosf(a - 0.8f) * bl, by + sinf(a - 0.8f) * bl, EPD_BLACK);
    }
  }
  g.fillCircle(cx, cy, q.R(3), EPD_RED);
  for (auto &p : {std::array<int, 2>{50, 8}, {52, 46}, {4, 50}}) g.fillCircle(q.X(p[0]), q.Y(p[1]), q.R(1.5f), EPD_BLACK);
}

void cake(EpdCanvas &g, const Grid &q) {
  g.fillRect(q.X(4), q.Y(52), q.R(48), q.R(3), EPD_BLACK);             // plate
  g.fillRect(q.X(8), q.Y(32), q.R(40), q.R(20), EPD_BLACK);            // cake
  g.fillRect(q.X(10), q.Y(34), q.R(36), q.R(16), EPD_WHITE);
  g.fillRect(q.X(10), q.Y(40), q.R(36), q.R(4), EPD_RED);              // jam layer
  g.fillRect(q.X(8), q.Y(30), q.R(40), q.R(5), EPD_RED);               // icing...
  for (int i = 0; i < 5; i++) g.fillCircle(q.X(12 + i * 8), q.Y(35), q.R(2.5f), EPD_RED);  // ...dripping
  for (int i = 0; i < 3; i++) {                                        // candles
    int cx = q.X(17 + i * 11);
    g.fillRect(cx - 1, q.Y(18), 3, q.Y(30) - q.Y(18), EPD_BLACK);
    flame(g, cx, q.Y(14), q.R(2));
  }
}
}  // namespace

// Wrap text to at most `width` characters per line; '\n' forces a break.
static int wrap(const String &in, int width, String out[], int max_lines) {
  int nl = in.indexOf('\n');
  if (nl >= 0) {
    int n = wrap(in.substring(0, nl), width, out, max_lines);
    return n + wrap(in.substring(nl + 1), width, out + n, max_lines - n);
  }
  String s = in;
  int n = 0;
  while (s.length() && n < max_lines) {
    int cut = (int)s.length() <= width ? s.length() : s.lastIndexOf(' ', width);
    if (cut <= 0) cut = min((int)s.length(), width);
    out[n++] = s.substring(0, cut);
    s = s.substring(cut);
    s.trim();
  }
  return n;
}

static void draw_holiday(EpdCanvas &g, const Holiday &h) {
  const int BX = 106, BY = 3, BS = 56, TX = 166, TW = 13;  // illustration box, text column
  Grid q{BX, BY, BS / 56.0f};
  switch (h.id) {
    case HOL_HALLOWEEN: pumpkin(g, q); break;
    case HOL_CHRISTMAS: xmas_tree(g, q); break;
    case HOL_NEW_YEAR: fireworks(g, q); break;
    case HOL_LUNAR_NY: lantern(g, q); break;
    case HOL_NOWRUZ: goldfish_bowl(g, q); break;
    case HOL_HIJRI_NY: crescent(g, q); break;
    case HOL_ROSH_HASHANAH: apple_honey(g, q); break;
    case HOL_SONGKRAN: water_gun(g, q); break;
    case HOL_VALENTINE: cupid_heart(g, q); break;
    case HOL_BIRTHDAY: cake(g, q); break;
    case HOL_EASTER: easter_egg(g, q); break;
    case HOL_DIWALI: diya(g, q); break;
    case HOL_HANUKKAH: menorah(g, q, h.arg); break;
    case HOL_EID_FITR: eid_lantern(g, q); break;
    case HOL_EID_ADHA: sheep(g, q); break;
    case HOL_HOLI: holi(g, q); break;
    case HOL_MID_AUTUMN: moon_cake(g, q); break;
    case HOL_THANKSGIVING: turkey(g, q); break;
    case HOL_ST_PATRICK: shamrock(g, q); break;
    case HOL_WOMENS_DAY: venus(g, q); break;
    case HOL_APRIL_FOOLS: jester(g, q); break;
    case HOL_PI_DAY: pi_pie(g, q); break;
    case HOL_SUMMER_SOLSTICE: cool_sun(g, q); break;
    case HOL_WINTER_SOLSTICE: snowflake(g, q); break;
  }
  String greet[5], quip[2];
  int ng = wrap(ascii_fold(h.greeting), TW, greet, 5), nq = wrap(ascii_fold(h.quip), TW, quip, 2);
  nq = min(nq, 5 - ng);  // 5 lines fit: long greetings (several birthday names) push the joke out
  int y = 33 - (ng * 11 + (nq ? 4 + nq * 10 : 0)) / 2;
  for (int i = 0; i < ng; i++, y += 11) text(g, TX, y, greet[i], EPD_RED);
  y += 4;
  for (int i = 0; i < nq; i++, y += 10) text(g, TX, y, quip[i], EPD_BLACK);
}

static void draw_days(EpdCanvas &g, const Weather &w) {
  const int X0 = 106, CW = 36, Y = 66;
  hdots(g, X0, 249, Y - 3, 2);
  for (int i = 0; i < Weather::DAYS; i++) {
    int x = X0 + i * CW, cx = x + CW / 2;
    String lbl = i == 0 ? "Yday" : i == 1 ? "Today" : WDAY[w.wday[i]];
    if (i == 1) {
      g.fillRect(x + 1, Y - 1, CW - 2, 10, EPD_RED);
      text_c(g, cx, Y, lbl, EPD_WHITE);
    } else {
      text_c(g, cx, Y, lbl, EPD_BLACK);
    }
    if (i > 0) vdots(g, x, Y, 121, EPD_BLACK, 3);
    icon(g, w.dcode[i], true, cx - 11, Y + 10, 22);
    g.cp437(true);
    text_c(g, cx, Y + 35, deg(w.tmax[i]), EPD_RED);
    text_c(g, cx, Y + 46, deg(w.tmin[i]), EPD_BLACK);
  }
}

void render(EpdCanvas &g, const Weather &w, const String &place, const Clock &clk, const Holiday *hol) {
  g.clear();
  g.setTextWrap(false);
  draw_current(g, w, place);
  draw_clock(g, clk);
  vdots(g, 103, 2, 119, EPD_BLACK, 2);
  if (hol && hol->id != HOL_NONE) draw_holiday(g, *hol);
  else draw_curve(g, w);
  draw_days(g, w);
  if (hol && hol->id == HOL_APRIL_FOOLS) g.rotate180();  // the prank
}

void render_icon_gallery(EpdCanvas &g) {
  static const int codes[] = {0, 2, 3, 45, 53, 63, 65, 73, 95, 0, 2};
  g.clear();
  for (int i = 0; i < 11; i++) {
    bool day = i < 9;
    int x = (i % 6) * 41, y = (i / 6) * 50;
    icon(g, codes[i], day, x, y, 40);
  }
  for (int i = 0; i < 9; i++) icon(g, codes[i], true, 124 + (i % 5) * 25, 100 - (i / 5) * 25 + 0, 22);
}

void render_status(EpdCanvas &g, const String lines[], int n) {
  g.clear();
  g.setTextWrap(false);
  g.cp437(true);
  g.fillRect(0, 0, 250, 30, EPD_BLACK);
  text_c(g, 125, 22, "Weather Station", EPD_WHITE, &FreeSansBold12pt7b);
  icon(g, 2, true, 6, 36, 70);  // partly cloudy
  int y0 = n > 4 ? 42 : 48, dy = n > 4 ? 12 : 14;  // squeeze to fit 5-6 lines
  for (int i = 0; i < n; i++) text(g, 86, y0 + i * dy, ascii_fold(lines[i]), EPD_BLACK);
  hdots(g, 0, 249, 119, 2);
  for (int i = 0; i < EpdCanvas::PLANE; i++) {  // no red: the fast refresh can't draw it
    g.black[i] &= ~g.red[i];
    g.red[i] = 0;
  }
}
