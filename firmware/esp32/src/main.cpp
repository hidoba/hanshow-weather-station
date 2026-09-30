// ESP32-S3 weather station driving a Hanshow 2.13" BWR e-paper tag over UART.
//  - weather (Open-Meteo) + PM2.5 every N minutes -> full 3-color refresh
//  - clock every minute -> fast black/white refresh
//  - settings page on http://<hostname>.local
// USB serial commands: r = full refresh now, d = dump framebuffer (hex, see tools/esp_preview.py)
#include "log.h"
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <time.h>
#include "app.h"
#include "config.h"
#include "render.h"
#include "secrets.h"
#include "swire.h"
#include "tag_firmware.h"

Settings settings;
Status status;
uint8_t preview[2][EpdCanvas::PLANE];
uint32_t preview_version;
volatile bool request_full = true;
uint8_t *volatile tag_fw = nullptr;
volatile size_t tag_fw_len = 0;
volatile bool tag_fw_sws = false;
volatile int preview_holiday = -1;
volatile int tag_action = TAG_NONE;

static SemaphoreHandle_t mtx;
void app_lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
void app_unlock() { xSemaphoreGive(mtx); }

static Tag tag(Serial1);
static EpdCanvas *canvas;

static void dump() {
  static const char hex[] = "0123456789abcdef";
  Serial.print("\nPLANES ");
  for (const uint8_t *p : {canvas->black, canvas->red})
    for (int i = 0; i < EpdCanvas::PLANE; i++) { Serial.write(hex[p[i] >> 4]); Serial.write(hex[p[i] & 15]); }
  Serial.println();
}

static bool wifi_up() { return WiFi.status() == WL_CONNECTED; }

static bool clock_valid() { return time(nullptr) > 1700000000; }

// Night level at night, day level by day, linear ramps centred on sunrise and sunset.
// Returns 0.1 % units.
static int led_level(const Weather &w, int minute_of_day) {
  auto ramp = [](int t, int centre) {  // 0 before, 1 after
    float f = (t - (centre - LED_RAMP_MIN / 2)) / (float)LED_RAMP_MIN;
    return constrain(f, 0.0f, 1.0f);
  };
  float day = ramp(minute_of_day, w.sunrise_min) * (1 - ramp(minute_of_day, w.sunset_min));
  return lroundf(10 * (LED_NIGHT_PCT + (LED_DAY_PCT - LED_NIGHT_PCT) * day));
}

// LED level right now, in 0.1 % units. Without a known local time and sunrise/sunset (e.g. booted
// during a Wi-Fi outage) assume night, so warnings never flash brightly in a dark bedroom.
static const Weather *sun_info;  // last good weather (sunrise/sunset, UTC offset), set by the worker

static int current_led() {
  if (!sun_info || !clock_valid()) return lroundf(LED_NIGHT_PCT * 10);
  time_t now = time(nullptr) + sun_info->utc_offset;
  struct tm t;
  gmtime_r(&now, &t);
  return led_level(*sun_info, t.tm_hour * 60 + t.tm_min);
}

// Status screens get their own canvas: they are drawn from setup() and the worker while the
// weather canvas may be in use. Guarded by the tag lock.
static EpdCanvas *scanvas;

// Fast-refresh a black/white status screen (power-on, updates, errors). Long lines are wrapped.
static bool show_status(std::initializer_list<String> lines, int led) {
  const unsigned WIDTH = 26;  // characters that fit right of the icon
  String l[6];
  int n = 0;
  for (String s : lines) {
    while (n < 6 && s.length() > WIDTH) {
      int cut = s.lastIndexOf(' ', WIDTH);
      if (cut <= 0) cut = WIDTH;
      l[n++] = s.substring(0, cut);
      s = s.substring(cut);
      s.trim();
    }
    if (n < 6 && s.length()) l[n++] = s;
  }
  tag.lock();
  render_status(*scanvas, l, n);
  bool ok = tag.show(*scanvas, true, led, true);
  tag.unlock();
  app_lock();
  status.tag_ok = ok;
  if (ok) {  // the preview mirrors what the panel actually shows
    memcpy(preview[0], scanvas->black, EpdCanvas::PLANE);
    memcpy(preview[1], scanvas->red, EpdCanvas::PLANE);
    preview_version++;
  }
  app_unlock();
  return ok;
}

// ---- messages posted from other tasks (web uploads, OTA), drawn by the worker ----
static String msg_lines[6];
static int msg_n;
static uint32_t msg_hold_s;
static volatile bool msg_pending;

void post_message(std::initializer_list<String> lines, uint32_t hold_s) {
  app_lock();
  msg_n = 0;
  for (auto &l : lines) if (msg_n < 6) msg_lines[msg_n++] = l;
  msg_hold_s = hold_s;
  msg_pending = true;
  app_unlock();
}

bool message_shown() { return !msg_pending; }

// ---- ESP32 onboard LED: what the tag can't show ----
enum EspLed { ESP_LED_OFF, ESP_LED_INSTALLING, ESP_LED_TAG_ERROR };
static volatile int esp_led = ESP_LED_OFF;

static void esp_led_task(void *) {
  for (int tick = 0;; tick++) {
    uint8_t v = max(2, (int)(current_led() * 255 / 1000));  // follows the day/night level
    bool on = (tick % 4) < 2;                                  // 1 Hz blink
    if (esp_led == ESP_LED_INSTALLING) neopixelWrite(STATUS_LED_PIN, 0, 0, (tick % 4) ? v : 0);
    else if (esp_led == ESP_LED_TAG_ERROR) neopixelWrite(STATUS_LED_PIN, on ? v : 0, 0, 0);
    else neopixelWrite(STATUS_LED_PIN, 0, 0, 0);
    delay(250);
  }
}

static int tag_version(const String &ping) {  // "OK P BWR213 v10 bank=1 n=0" -> 10
  int i = ping.indexOf(" v");
  return i < 0 ? -1 : ping.substring(i + 2).toInt();
}

static void fw_status(const String &s) {
  logln("tag: " + s);
  app_lock(); status.fw_status = s; app_unlock();
}

// Make sure the tag runs the firmware bundled in this image:
//  - no reply at all (factory Hanshow firmware, blank or broken tag): flash it over SWS
//  - older version: update over the UART link (SWS as fallback)
// Returns a note for the splash screen ("" if nothing happened).
static String provision_tag() {
  String info;
  tag.lock();
  bool alive = false;
  for (int i = 0; i < 10 && !(alive = tag.ping(&info)); i++) delay(300);  // tag boots with us
  if (!alive) { tag_hard_reset(); delay(2000); alive = tag.ping(&info); }
  int v = alive ? tag_version(info) : -1;
  tag.unlock();
  if (alive && v >= TAG_FW_BUNDLED_VERSION) return "";
  auto prog = [](int pct) { app_lock(); status.fw_status = "installing tag firmware: " + String(pct) + "%"; app_unlock(); };
  bool ok = false;
  String msg;
  if (alive) {
    fw_status("updating tag v" + String(v) + " -> v" + String(TAG_FW_BUNDLED_VERSION));
    show_status({"Updating tag firmware", "v" + String(v) + " -> v" + String(TAG_FW_BUNDLED_VERSION), "please wait..."},
                LED_DAY_PCT * 10);
    tag.lock();
    ok = tag.update_firmware(TAG_FW_IMAGE, sizeof TAG_FW_IMAGE, msg, prog);
    tag.unlock();
  }
  if (!ok) {  // the tag can't show anything now: the ESP32's own LED pulses blue meanwhile
    fw_status(alive ? "UART update failed (" + msg + "), trying SWS" : "tag not responding: installing firmware over SWS");
    esp_led = ESP_LED_INSTALLING;
    tag.lock();
    swire_flash(TAG_FW_IMAGE, sizeof TAG_FW_IMAGE, prog);
    for (int i = 0; i < 10 && !(ok = tag.ping(&info)); i++) delay(500);
    tag.unlock();
    msg = info;
  }
  esp_led = ok ? ESP_LED_OFF : ESP_LED_TAG_ERROR;
  msg.replace("OK P ", "");
  fw_status(ok ? "tag firmware installed: " + msg : "tag firmware install FAILED - check the wiring (NRST, SWS)");
  return ok ? "Tag firmware v" + String(TAG_FW_BUNDLED_VERSION) + " installed" : "";
}

static void show_wifi_screen() {
  show_status({"Connecting to Wi-Fi", "\"" WIFI_SSID "\"", "please wait..."}, current_led());
}

static void show_server_screen() {
  app_lock();
  String err = status.last_error;
  app_unlock();
  show_status({"Wi-Fi ok " + WiFi.localIP().toString(), "Can't reach weather server, retrying...", err}, current_led());
}

static void flash_red(int times = 1) {
  tag.lock();
  for (int i = 0; i < times; i++) {
    if (i) delay(200);
    tag.led(current_led(), times > 1 ? 150 : 300, true);  // dimmed at night like the green LED
  }
  tag.unlock();
}

static void set_error(const String &e) {
  logln("error: " + e);
  app_lock(); status.last_error = e; app_unlock();
}

// Fetch weather + air quality for the configured place. Resolves the city first if needed.
static bool fetch(Weather &w, String &place) {
  if (!wifi_up()) { set_error("Wi-Fi not connected"); return false; }
  app_lock();
  Settings s = settings;
  app_unlock();
  if (isnan(s.lat)) {
    Place p;
    if (!geocode(s.city, p)) {
      static String reported;
      set_error("City not found: " + s.city);
      if (reported != s.city) {  // tell once per city, not on every retry
        reported = s.city;
        post_message({"City not found:", s.city, "change it on the settings page"}, MESSAGE_HOLD_S);
      }
      return false;
    }
    s.place = p.name; s.lat = p.lat; s.lon = p.lon;
    app_lock();
    if (settings.city == s.city) { settings.place = p.name; settings.lat = p.lat; settings.lon = p.lon; settings_save(settings); }
    app_unlock();
  }
  if (!fetch_weather(s.lat, s.lon, s.fahrenheit, w)) { set_error("Weather fetch failed"); return false; }
  fetch_air(s.lat, s.lon, w);  // optional
  place = s.place;
  return true;
}

static void worker(void *) {
  Weather wx;
  String place;
  bool have = false;
  uint32_t fetch_at = 0, fetch_wait = 0;  // next fetch due when millis() - fetch_at >= fetch_wait (wrap-safe)
  int shown_minute = -1;
  String shown_holiday = "-";  // holiday content on the display: any change needs a full refresh (red)
  Clock clk = {};
  uint32_t wifi_down_since = millis(), last_flash = 0, last_ok = 0, last_wifi_kick = millis();
  bool wifi_screen = false, server_fail = false, server_screen = false;
  bool holding = false;      // a message is on the display...
  uint32_t hold_start = 0, hold_ms = 0;  // ...since hold_start, for hold_ms (unsigned math: wrap-safe)
  for (int i = 0; i < 20 && !wifi_up(); i++) delay(250);
  for (int i = 0; i < 20 && wifi_up() && !clock_valid(); i++) delay(250);  // let NTP sync before the first draw
  for (;;) {
    if (msg_pending) {
      app_lock();
      String l[6];
      int n = msg_n;
      for (int i = 0; i < n; i++) l[i] = msg_lines[i];
      uint32_t hold = msg_hold_s;
      app_unlock();
      show_status({l[0], l[1], l[2], l[3], l[4], l[5]}, current_led());
      holding = true;
      hold_start = millis();
      hold_ms = hold * 1000UL;
      msg_pending = false;
    }
    if (wifi_up()) {
      wifi_down_since = last_wifi_kick = millis();
      wifi_screen = false;
    } else if (millis() - last_wifi_kick > WIFI_RETRY_S * 1000UL) {
      logln("wifi: still down, reconnecting");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      last_wifi_kick = millis();
    }
    if (!wifi_up() && millis() - wifi_down_since > WIFI_WARN_S * 1000UL) {
      if (!have && !wifi_screen) { show_wifi_screen(); wifi_screen = true; }
      if (millis() - last_flash > 2000) { flash_red(); last_flash = millis(); }
    }
    if (wifi_up() && server_fail) {
      if (!have && !server_screen) { show_server_screen(); server_screen = true; }
      if (millis() - last_flash > 4000) { flash_red(2); last_flash = millis(); }
    }
    if (tag_fw) {  // firmware update for the tag, uploaded via the web page
      app_lock(); status.fw_status = "updating tag: 0%"; app_unlock();
      String msg;
      tag.lock();
      auto prog = [](int pct) { app_lock(); status.fw_status = "updating tag: " + String(pct) + "%"; app_unlock(); };
      bool ok;
      tag.unlock();
      show_status({"Updating tag firmware", tag_fw_sws ? "recovery flash over SWS" : "received over Wi-Fi",
                   "please wait..."}, current_led());
      if (tag_fw_sws) esp_led = ESP_LED_INSTALLING;
      tag.lock();
      if (tag_fw_sws) {  // recovery path: raw flash write over SWS, works even if the tag firmware is broken
        ok = swire_flash(tag_fw, tag_fw_len, prog);
        String info;
        for (int i = 0; ok && i < 10 && !tag.ping(&info); i++) delay(500);
        ok = ok && info.startsWith("OK P");
        msg = ok ? info + " (via SWS)" : "no reply after SWS flash";
      } else {
        ok = tag.update_firmware(tag_fw, tag_fw_len, msg, prog);
      }
      tag.unlock();
      free(tag_fw);
      tag_fw = nullptr;
      logln("tag update: " + msg);
      app_lock(); status.fw_status = ok ? "tag updated: " + msg : "tag update FAILED: " + msg; app_unlock();
      esp_led = ESP_LED_OFF;
      String shortmsg = msg;
      shortmsg.replace("OK P ", "");
      if (ok) post_message({"Tag firmware updated", "now running v" + String(tag_version(msg))}, 10);
      else post_message({"Tag update FAILED:", shortmsg, "the old firmware keeps running"}, MESSAGE_HOLD_S);
    }
    if (tag_action != TAG_NONE) {
      int a = tag_action;
      tag_action = TAG_NONE;
      tag.lock();
      String r;
      if (a == TAG_RESET) {
        tag_hard_reset();
        delay(2000);
        r = tag.ping() ? "tag reset ok" : "tag reset: no reply";
      } else if (a == TAG_DEMO_WIFI || a == TAG_DEMO_SERVER) {  // preview of the error states
        tag.unlock();
        bool w = a == TAG_DEMO_WIFI;
        w ? show_wifi_screen() : show_server_screen();
        for (int i = 0; i < 3; i++) { flash_red(w ? 1 : 2); delay(w ? 1700 : 3500); }
        tag.lock();
        r = String(w ? "wifi" : "server") + " screen demo done (LED " + String(current_led() / 10.0, 1) + "%)";
      } else {  // SWS link test: halt the CPU via SWS (tag must go silent), then let it run again
        bool before = tag.ping();
        swire_halt_cpu();
        bool halted = !tag.ping();
        swire_release();
        delay(2000);
        bool after = tag.ping();
        r = String("SWS test: before ") + (before ? "ok" : "-") + ", halted " + (halted ? "yes" : "NO") +
            ", after " + (after ? "ok" : "-") + (before && halted && after ? " -> PASS" : " -> FAIL");
      }
      tag.unlock();
      logln(r);
      app_lock(); status.fw_status = r; app_unlock();
      request_full = true;  // the tag rebooted: redraw everything
    }
    bool full = false;
    if (wifi_up() && (request_full || millis() - fetch_at >= fetch_wait)) {
      bool forced = request_full;
      request_full = false;
      logf("[%lus] fetch (%s)\n", millis() / 1000, forced ? "requested" : "scheduled");
      Weather nw;
      String np;
      if (fetch(nw, np)) {
        wx = nw; place = np; have = true;
        sun_info = &wx;
        time_t now = time(nullptr) + wx.utc_offset;
        struct tm t; gmtime_r(&now, &t);
        clk.upd_hour = clock_valid() ? t.tm_hour : wx.hour;
        clk.upd_min = clock_valid() ? t.tm_min : wx.minute;
        app_lock();
        status.wx = wx; status.have_weather = true; status.last_error = ""; status.last_fetch = millis();
        int mins = settings.weather_minutes;
        app_unlock();
        fetch_at = millis();
        fetch_wait = max(mins, 1) * 60000UL;
        full = true;
        server_fail = server_screen = false;
        last_ok = millis();
        clk.stale = false;
      } else {
        fetch_at = millis();
        fetch_wait = have ? RETRY_MINUTES * 60000UL : FIRST_RETRY_S * 1000UL;
        full = forced && have;  // still redraw if settings changed
        server_fail = wifi_up();
        app_lock();
        int mins = settings.weather_minutes;
        app_unlock();
        clk.stale = have && millis() - last_ok > 3UL * max(mins, 5) * 60000UL;
      }
    }
    if (preview_holiday >= 0 && have) {  // settings page: show a holiday's illustration for a minute
      time_t now = time(nullptr) + wx.utc_offset;
      struct tm t;
      gmtime_r(&now, &t);
      HolidayContext hctx;
      static Birthday bdays[MAX_BIRTHDAYS];
      app_lock();
      for (int i = 0; i < MAX_BIRTHDAYS; i++) bdays[i] = settings.birthdays[i];
      hctx.south = settings.lat < 0;
      app_unlock();
      hctx.birthdays = bdays;
      hctx.n_birthdays = MAX_BIRTHDAYS;
      Holiday demo = clock_valid() ? holiday_next(preview_holiday, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, hctx)
                                   : holiday_next(preview_holiday, 0, 0, 0, hctx);
      preview_holiday = -1;
      render(*canvas, wx, place, clk, &demo);
      tag.lock();
      bool ok = tag.show(*canvas, false, current_led());
      tag.unlock();
      if (ok) {
        app_lock();
        memcpy(preview[0], canvas->black, EpdCanvas::PLANE);
        memcpy(preview[1], canvas->red, EpdCanvas::PLANE);
        preview_version++;
        app_unlock();
      }
      holding = true;
      hold_start = millis();
      hold_ms = MESSAGE_HOLD_S * 1000UL;
    }
    if (holding && millis() - hold_start >= hold_ms) {  // message expired: back to the weather
      holding = false;
      full = true;
    }
    if (have && !holding) {
      app_lock();
      clk.show = settings.clock && clock_valid();
      bool holidays = settings.holidays;
      HolidayContext hctx;
      static Birthday bdays[MAX_BIRTHDAYS];
      for (int i = 0; i < MAX_BIRTHDAYS; i++) bdays[i] = settings.birthdays[i];
      hctx.birthdays = bdays;
      hctx.n_birthdays = MAX_BIRTHDAYS;
      hctx.south = settings.lat < 0;
      app_unlock();
      int minute = -1;
      time_t now = time(nullptr) + wx.utc_offset;
      struct tm t; gmtime_r(&now, &t);
      int led = current_led();
      if (clk.show) {
        clk.hour = t.tm_hour; clk.minute = t.tm_min;
        minute = t.tm_hour * 60 + t.tm_min;
      }
      bool dated = clock_valid();
      clk.year = dated ? t.tm_year + 1900 : 0;
      clk.month = dated ? t.tm_mon + 1 : 0;
      clk.day = dated ? t.tm_mday : 0;
      Holiday hol = holidays && dated ? holiday_for(clk.year, clk.month, clk.day, hctx) : Holiday{HOL_NONE, "", "", 0};
      String hol_key = String(hol.id) + '|' + hol.greeting + '|' + hol.quip + '|' + hol.arg;
      if (hol_key != shown_holiday) full = true;  // e.g. Christmas Eve -> Day, a new Hanukkah candle
      if (full || (clk.show && minute != shown_minute)) {
        logf("[%lus] draw %02d:%02d %s\n", millis() / 1000, clk.hour, clk.minute, full ? "full" : "fast");
        render(*canvas, wx, place, clk, &hol);
        tag.lock();
        bool ok = tag.show(*canvas, !full, led);
        tag.unlock();
        if (esp_led != ESP_LED_INSTALLING) esp_led = ok ? ESP_LED_OFF : ESP_LED_TAG_ERROR;
        app_lock();
        status.tag_ok = ok;
        status.tag_info = tag.info();
        status.led = led;
        if (ok) {
          memcpy(preview[0], canvas->black, EpdCanvas::PLANE);
          memcpy(preview[1], canvas->red, EpdCanvas::PLANE);
          preview_version++;
          (full ? status.last_full : status.last_fast) = millis();
        } else {
          status.last_error = "Tag not responding - reboot the ESP32 to reinstall the tag firmware";
        }
        app_unlock();
        if (ok) { shown_minute = minute; shown_holiday = hol_key; }
        else if (full) { fetch_at = millis(); fetch_wait = RETRY_MINUTES * 60000UL; }
      }
    }
    delay(500);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // never stall on logging when no USB host is reading
  mtx = xSemaphoreCreateMutex();
  swire_begin();
  tag.begin(TAG_RX_PIN, TAG_TX_PIN);
  canvas = new EpdCanvas();
  scanvas = new EpdCanvas();
  if (STATUS_LED_PIN >= 0) xTaskCreatePinnedToCore(esp_led_task, "esp_led", 2048, nullptr, 1, nullptr, 0);
  delay(1500);
  logln("\nhanshow weather");
  settings_load(settings);
  logf("city: %s  host: %s.local  every %d min  clock %d\n", settings.city.c_str(),
                settings.hostname.c_str(), settings.weather_minutes, settings.clock);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(settings.hostname.c_str());
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);  // connects in the background
  String note = provision_tag();
  show_status({"Starting up...", "fw " FW_VERSION, note}, LED_DAY_PCT * 10);  // visible "it's on" blink
  configTime(0, 0, "pool.ntp.org", "time.google.com");  // UTC; local offset comes from Open-Meteo
  web_begin();
  ArduinoOTA.setHostname(settings.hostname.c_str());
  ArduinoOTA.setMdnsEnabled(false);  // web.cpp owns mDNS
  if (*ADMIN_PASSWORD) ArduinoOTA.setPassword(ADMIN_PASSWORD);
  ArduinoOTA.setRebootOnSuccess(false);  // restart ourselves once the display says so
  ArduinoOTA.onStart([] {
    logln("ota: esp32 update started");
    post_message({"Receiving new firmware", "for the ESP32...", "don't unplug"}, 600);
  });
  ArduinoOTA.onEnd([] {
    post_message({"ESP32 firmware updated", "restarting..."}, 600);
    for (int i = 0; i < 40 && !message_shown(); i++) delay(100);
    ESP.restart();
  });
  ArduinoOTA.onError([](ota_error_t e) {
    static const char *why[] = {"authentication failed", "begin failed", "connection lost", "receive failed", "end failed"};
    post_message({"ESP32 update FAILED:", e <= OTA_END_ERROR ? why[e] : "unknown error", "still running the old firmware"}, MESSAGE_HOLD_S);
  });
  ArduinoOTA.begin();
  xTaskCreatePinnedToCore(worker, "worker", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
  web_loop();
  ArduinoOTA.handle();
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'r': request_full = true; break;
      case 'd': dump(); break;
    }
  }
  delay(2);
}
