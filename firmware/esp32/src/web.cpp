// Settings page: http://<hostname>.local
#include "log.h"
#include <ESPmDNS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "app.h"
#include "config.h"
#include "holidays.h"
#include "secrets.h"

static WebServer server(80);
static String mdns_name;

#ifndef ADMIN_PASSWORD
#error "secrets.h needs ADMIN_PASSWORD (see secrets.example.h)"
#endif

// HTTP basic auth (user "admin") for every page and action, unless ADMIN_PASSWORD is "".
static bool authorized() { return !*ADMIN_PASSWORD || server.authenticate("admin", ADMIN_PASSWORD); }

static bool require_auth() {
  if (authorized()) return true;
  server.requestAuthentication(BASIC_AUTH, "Weather Station");
  return false;
}

// Wraps a handler so it only runs for an authenticated client.
template <typename F> static std::function<void()> guarded(F f) {
  return [f] { if (require_auth()) f(); };
}

static String esc(const String &s) {
  String o;
  for (char c : s) {
    if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;"; else o += c;
  }
  return o;
}

// Age of a millis() timestamp; the page's script keeps these ticking between updates.
static String ago(uint32_t t) {
  if (!t) return "never";
  long d = (millis() - t) / 1000;
  String txt = d < 90 ? String(d) + " s ago" : d < 5400 ? String(d / 60) + " min ago" : String(d / 3600) + " h ago";
  return "<span class=age data-s=" + String(d) + ">" + txt + "</span>";
}

static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Weather display</title>
<style>
body{font-family:system-ui,sans-serif;max-width:560px;margin:0 auto;padding:16px;background:#f4f4f2;color:#222}
h1{font-size:1.3em;margin:.2em 0 .6em}
.card{background:#fff;border-radius:10px;padding:14px 16px;margin-bottom:14px;box-shadow:0 1px 3px #0002}
canvas{width:100%;image-rendering:pixelated;border:1px solid #ccc;border-radius:4px}
label{display:block;margin:.7em 0 .2em;font-weight:600;font-size:.9em}
input[type=text],input[type=number],select{width:100%;box-sizing:border-box;padding:8px;font-size:1em;border:1px solid #bbb;border-radius:6px}
.row{display:flex;gap:10px;margin-top:14px}
button{flex:1;padding:10px;font-size:1em;border:0;border-radius:6px;background:#c62828;color:#fff;cursor:pointer}
button.sec{background:#444}
.muted{color:#777;font-size:.85em}
.bd{display:flex;gap:6px;margin:3px 0}.bd input{flex:2}.bd select{flex:1;width:auto}
.pv select{flex:1;width:auto}.pv button{flex:0 0 auto;white-space:nowrap}.err{color:#c62828;font-weight:600}
td{padding:2px 12px 2px 0}
</style></head><body>
<h1>Weather display</h1>
<div class="card"><canvas id="c" width="250" height="122"></canvas>
<table class="muted" id="st">%STATUS%</table></div>
<form class="card" method="post" action="/save">
%SETTINGSMSG%
<label>City</label><input type="text" name="city" value="%CITY%" required>
<div class="muted">%PLACE%</div>
<label>Weather update / full refresh (minutes)</label><input type="number" name="wmin" min="1" max="120" value="%WMIN%">
<label>Units</label><select name="units"><option value="c" %UNITC%>&deg;C, km/h</option><option value="f" %UNITF%>&deg;F, mph</option></select>
<label><input type="checkbox" name="clock" %CLOCK%> Show clock (fast refresh every minute)</label>
<label><input type="checkbox" name="psave" %PSAVE%> Power saving: deep sleep when this page hasn't been opened for 5 minutes (the clock and weather keep updating; press BOOT on the ESP32 or power-cycle it to reach this page again)</label>
<label><input type="checkbox" name="holidays" %HOLIDAYS%> Holidays: an illustration instead of the temperature curve on special days</label>
<label>Birthdays (shown on the day, before any other holiday)</label>
%BIRTHDAYS%
<div class="row pv" style="margin-top:6px"><select name="pid">%HOLIDAYOPTS%</select>
<button class="sec" type="submit" formaction="/holiday" formnovalidate>Preview on display</button></div>
<label>Hostname</label><input type="text" name="host" value="%HOST%" pattern="[a-z0-9-]+">
<div class="muted">This page: http://%HOST%.local</div>
<div class="row"><button type="submit">Save</button>
<button class="sec" type="submit" formaction="/refresh">Refresh now</button></div>
</form>

<div class="card"><b>Firmware</b>
<table class="muted"><tr><td>ESP32</td><td>%BUILD%</td></tr><tr><td>Tag</td><td>%TAGFW%</td></tr>%FWSTATUS%</table>
<form method="post" action="/update/esp32" enctype="multipart/form-data">
<label>ESP32 firmware (firmware/esp32/.pio/build/esp32s3/firmware.bin)</label>
<input type="file" name="fw" accept=".bin" required><div class="row"><button class="sec" type="submit">Update ESP32</button></div></form>
<form method="post" action="/update/tag" enctype="multipart/form-data">
<label>Tag firmware (firmware/tag/hanshow_serial.bin)</label>
<input type="file" name="fw" accept=".bin" required>
<label><input type="checkbox" name="sws"> Recovery flash over SWS (use if the tag doesn't respond)</label>
<div class="row"><button class="sec" type="submit">Update tag</button></div></form>
<form method="post"><div class="row">
<button class="sec" type="submit" formaction="/tag/reset">Reset tag</button>
<button class="sec" type="submit" formaction="/tag/swstest">Test SWS link</button></div></form>
</div>
<script>
%RELOAD%
// Live preview: redraw whenever the ESP32 pushes a new image to the display.
const cv=document.getElementById('c').getContext('2d');
let ver=-1;
function draw(b){
 const d=new Uint8Array(b),im=cv.createImageData(250,122);
 for(let x=0;x<250;x++)for(let y=0;y<122;y++){
  const i=(y>>3)+(249-x)*16,m=0x80>>(y&7),o=(y*250+x)*4;
  const col=(d[4000+i]&m)?[200,30,30]:(d[i]&m)?[255,255,255]:[20,20,20];
  im.data.set([...col,255],o);}
 cv.putImageData(im,0,0);}
const st=document.getElementById('st');
let base=Date.now(),lastStatus=Date.now();
function fmt(d){return d<90?d+' s ago':d<5400?Math.floor(d/60)+' min ago':Math.floor(d/3600)+' h ago';}
function tick(){  // advance the "... ago" values every second
 const e=Math.floor((Date.now()-base)/1000);
 st.querySelectorAll('.age').forEach(a=>a.textContent=fmt(+a.dataset.s+e));}
async function loadStatus(){st.innerHTML=await (await fetch('/status')).text();base=lastStatus=Date.now();}
async function poll(){
 try{
  const v=+(await (await fetch('/version')).text());
  if(v!==ver){
   draw(await (await fetch('/planes')).arrayBuffer());
   if(ver>=0)await loadStatus();
   ver=v;
  }else if(Date.now()-lastStatus>10000)await loadStatus();
 }catch(e){}
 setTimeout(poll,2000);}
setInterval(tick,1000);
poll();
</script></body></html>)HTML";

static String status_rows() {
  app_lock();
  Status st = status;
  app_unlock();
  String rows;
  auto row = [&](const String &k, const String &v) { rows += "<tr><td>" + k + "</td><td>" + v + "</td></tr>"; };
  if (st.have_weather) {
    row("Now", String(st.wx.temp, 1) + (st.wx.fahrenheit ? " &deg;F, " : " &deg;C, ") + String(st.wx.humidity) + "% humidity");
    if (!isnan(st.wx.pm25)) row("PM2.5", String(st.wx.pm25, 1) + " &micro;g/m&sup3; (EU AQI " + String(st.wx.eaqi) + ")");
  }
  row("Weather fetched", ago(st.last_fetch));
  row("Full refresh", ago(st.last_full));
  row("Clock refresh", ago(st.last_fast));
  row("Tag", st.tag_ok ? "ok" : "<span class=err>not responding</span>");
  if (st.have_weather) {
    char sun[48];
    snprintf(sun, sizeof sun, "%.1f%% (sunrise %02d:%02d, sunset %02d:%02d)", st.led / 10.0, st.wx.sunrise_min / 60,
             st.wx.sunrise_min % 60, st.wx.sunset_min / 60, st.wx.sunset_min % 60);
    row("LED", sun);
  }
  row("IP", WiFi.localIP().toString());
  app_lock();
  bool ps = settings.power_save;
  app_unlock();
  long left = (long)AWAKE_WINDOW_S - (long)((millis() - awake_since) / 1000);
  row("Power", !ps ? String("always on") :
                     "deep sleep in " + String(max(0L, left) / 60) + ":" + (max(0L, left) % 60 < 10 ? "0" : "") +
                     String(max(0L, left) % 60) + " (reload the page to stay awake; afterwards press BOOT or power-cycle)");
  if (st.last_error.length()) row("Error", "<span class=err>" + esc(st.last_error) + "</span>");
  return rows;
}

static void handle_root() {
  awake_since = millis();  // opening the page keeps the ESP32 awake (the page's own polling doesn't)
  app_lock();
  Settings s = settings;
  Status st = status;
  app_unlock();
  String rows = status_rows();

  String page = FPSTR(PAGE);
  page.replace("%STATUS%", rows);
  page.replace("%CITY%", esc(s.city));
  page.replace("%PLACE%", isnan(s.lat) ? String("resolving&hellip;")
                                       : esc(s.place) + " (" + String(s.lat, 3) + ", " + String(s.lon, 3) + ")");
  page.replace("%WMIN%", String(s.weather_minutes));
  page.replace("%CLOCK%", s.clock ? "checked" : "");
  page.replace("%HOLIDAYS%", s.holidays ? "checked" : "");
  page.replace("%PSAVE%", s.power_save ? "checked" : "");
  page.replace("%SETTINGSMSG%", st.settings_msg.length() ? "<div class=err>" + esc(st.settings_msg) + "</div>" : String(""));
  static const char *MON[] = {"-", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  String bd;
  for (int i = 0; i < MAX_BIRTHDAYS; i++) {
    const Birthday &b = s.birthdays[i];
    bd += "<div class=bd><input type=text maxlength=20 placeholder=Name name=bn" + String(i) + " value=\"" + esc(b.name) +
          "\"><select name=bm" + String(i) + ">";
    for (int m = 0; m <= 12; m++) bd += "<option value=" + String(m) + (b.month == m ? " selected>" : ">") + MON[m] + "</option>";
    bd += "</select><select name=bd" + String(i) + ">";
    for (int d = 0; d <= 31; d++) bd += "<option value=" + String(d) + (b.day == d ? " selected>" : ">") + (d ? String(d) : "-") + "</option>";
    bd += "</select></div>";
  }
  page.replace("%BIRTHDAYS%", bd);
  String opts;
  for (int id = HOL_NONE + 1; id < HOL_COUNT; id++) opts += "<option value=" + String(id) + ">" + holiday_name(id) + "</option>";
  page.replace("%HOLIDAYOPTS%", opts);
  page.replace("%UNITC%", s.fahrenheit ? "" : "selected");
  page.replace("%UNITF%", s.fahrenheit ? "selected" : "");
  page.replace("%HOST%", esc(s.hostname));
  page.replace("%BUILD%", "v" FW_VERSION ", " __DATE__ " " __TIME__);
  String tf = st.tag_info;  // "OK P BWR213 v7 bank=0 n=3"
  tf.replace("OK P ", "");
  page.replace("%TAGFW%", tf.length() ? esc(tf) : String("unknown"));
  bool updating = st.fw_status.startsWith("updating") || tag_fw || tag_action != TAG_NONE;
  page.replace("%FWSTATUS%", st.fw_status.length() ? "<tr><td>Update</td><td>" + esc(st.fw_status) + "</td></tr>" : String(""));
  page.replace("%RELOAD%", updating ? "setTimeout(()=>location.reload(),2000);" : "");
  server.send(200, "text/html; charset=utf-8", page);
}

// ---- firmware uploads ----

static String upload_error;

static void esp32_upload() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    upload_error = "";
    if (!authorized()) { upload_error = "unauthorized"; return; }
    logf("ota: esp32 upload %s\n", u.filename.c_str());
    esp_wifi_set_ps(WIFI_PS_NONE);  // full speed for the transfer (restored by the reboot)
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) upload_error = Update.errorString();
    else post_message({"Receiving new firmware", "for the ESP32...", "don't unplug"}, 600);
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (upload_error.isEmpty() && Update.write(u.buf, u.currentSize) != u.currentSize) upload_error = Update.errorString();
  } else if (u.status == UPLOAD_FILE_END) {
    if (upload_error.isEmpty() && !Update.end(true)) upload_error = Update.errorString();
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    // The client went away mid-upload; WebServer won't call esp32_done, so report it here.
    if (upload_error.isEmpty()) {
      Update.abort();
      post_message({"ESP32 update FAILED:", "upload interrupted", "still running the old firmware"}, MESSAGE_HOLD_S);
    }
    upload_error = "upload aborted";
  }
}

static void esp32_done() {
  if (!require_auth()) return;
  if (upload_error.length()) {
    if (upload_error != "unauthorized")
      post_message({"ESP32 update FAILED:", upload_error, "still running the old firmware"}, MESSAGE_HOLD_S);
    server.send(500, "text/plain", "ESP32 update failed: " + upload_error);
    return;
  }
  server.send(200, "text/html", "<meta http-equiv=refresh content='15;url=/'>ESP32 updated, rebooting&hellip;");
  post_message({"ESP32 firmware updated", "restarting..."}, 600);
  for (int i = 0; i < 40 && !message_shown(); i++) delay(100);  // let the display say so first
  ESP.restart();
}

static uint8_t *tag_buf;
static size_t tag_len;
static const size_t TAG_MAX = 124 * 1024;

static void tag_upload() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    upload_error = "";
    free(tag_buf);
    tag_buf = nullptr;
    tag_len = 0;
    if (!authorized()) { upload_error = "unauthorized"; return; }
    tag_buf = (uint8_t *)ps_malloc(TAG_MAX);
    if (!tag_buf) upload_error = "out of memory";
    if (tag_fw) upload_error = "a tag update is already running";
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (upload_error.isEmpty()) {
      if (tag_len + u.currentSize > TAG_MAX) upload_error = "file too large";
      else { memcpy(tag_buf + tag_len, u.buf, u.currentSize); tag_len += u.currentSize; }
    }
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    upload_error = "upload aborted";
  }
}

static void tag_done() {
  if (!require_auth()) { free(tag_buf); tag_buf = nullptr; return; }
  if (upload_error.isEmpty() && (tag_len < 16 || memcmp(tag_buf + 8, "KNLT", 4)))
    upload_error = "not a Telink firmware image";
  if (upload_error.length()) {
    free(tag_buf);
    tag_buf = nullptr;
    post_message({"Tag update rejected:", upload_error}, MESSAGE_HOLD_S);
    server.send(400, "text/plain", "Tag update rejected: " + upload_error);
    return;
  }
  logf("ota: tag image %u bytes\n", (unsigned)tag_len);
  tag_fw_len = tag_len;
  tag_fw_sws = server.hasArg("sws");
  tag_fw = tag_buf;  // the worker flashes it and frees the buffer
  tag_buf = nullptr;
  app_lock(); status.fw_status = "updating tag: queued"; app_unlock();
  server.sendHeader("Location", "/");
  server.send(303);
}

static void start_mdns(const String &name) {
  if (mdns_name.length()) MDNS.end();
  mdns_name = name;
  if (MDNS.begin(name.c_str())) {
    MDNS.addService("http", "tcp", 80);
    logf("web: http://%s.local\n", name.c_str());
  }
}

static void handle_save() {
  logf("web: save from %s\n", server.client().remoteIP().toString().c_str());
  String city = server.arg("city"), host = server.arg("host");
  city.trim();
  host.trim();
  host.toLowerCase();
  int wmin = constrain(server.arg("wmin").toInt(), 1, 120);
  bool clock = server.hasArg("clock");
  bool host_changed;
  app_lock();
  if (city.length() && city != settings.city) {
    settings.city = city;
    settings.lat = settings.lon = NAN;  // worker geocodes on its next fetch
    settings.place = "";
  }
  settings.weather_minutes = wmin;
  settings.clock = clock;
  settings.holidays = server.hasArg("holidays");
  settings.power_save = server.hasArg("psave");
  String bad_dates;
  for (int i = 0; i < MAX_BIRTHDAYS; i++) {
    String n = server.arg("bn" + String(i));
    n.trim();
    n.replace("|", "/");  // the storage separator
    int m = constrain(server.arg("bm" + String(i)).toInt(), 0, 12), d = constrain(server.arg("bd" + String(i)).toInt(), 0, 31);
    static const int DAYS_IN[13] = {0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};  // Feb 29 allowed
    if (n.length() && m && d > DAYS_IN[m]) {
      static const char *MON[] = {"", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
      bad_dates += (bad_dates.length() ? ", " : "") + n + " (" + MON[m] + " " + d + ")";
      d = 0;  // not saved
    }
    settings.birthdays[i] = n.length() && m && d ? Birthday{n.substring(0, 20), (uint8_t)m, (uint8_t)d} : Birthday{"", 0, 0};
  }
  settings.fahrenheit = server.arg("units") == "f";
  host_changed = host.length() && host != settings.hostname;
  if (host_changed) settings.hostname = host;
  settings_save(settings);
  status.settings_msg = bad_dates.length() ? "Birthday not saved, no such date: " + bad_dates : "";
  app_unlock();
  if (host_changed) {
    WiFi.setHostname(host.c_str());
    start_mdns(host);
  }
  request_full = true;
  server.sendHeader("Location", host_changed ? "http://" + host + ".local/" : String("/"));
  server.send(303);
}

static void handle_refresh() {
  logf("web: refresh from %s\n", server.client().remoteIP().toString().c_str());
  request_full = true;
  server.sendHeader("Location", "/");
  server.send(303);
}

static void tag_action_req(int a, const char *msg) {
  tag_action = a;
  app_lock(); status.fw_status = msg; app_unlock();
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handle_planes() {
  uint8_t *buf = (uint8_t *)malloc(2 * EpdCanvas::PLANE);
  if (!buf) { server.send(500); return; }
  app_lock();
  memcpy(buf, preview, 2 * EpdCanvas::PLANE);
  app_unlock();
  server.send_P(200, "application/octet-stream", (const char *)buf, 2 * EpdCanvas::PLANE);
  free(buf);
}

void web_begin() {
  server.on("/", HTTP_GET, guarded(handle_root));
  server.on("/save", HTTP_POST, guarded(handle_save));
  server.on("/refresh", HTTP_POST, guarded(handle_refresh));
  server.on("/planes", HTTP_GET, guarded(handle_planes));
  server.on("/version", HTTP_GET, guarded([] {
    app_lock();
    uint32_t v = preview_version;
    app_unlock();
    server.send(200, "text/plain", String(v));
  }));
  server.on("/status", HTTP_GET, guarded([] { server.send(200, "text/html; charset=utf-8", status_rows()); }));
  server.on("/update/esp32", HTTP_POST, esp32_done, esp32_upload);  // auth checked in both callbacks
  server.on("/update/tag", HTTP_POST, tag_done, tag_upload);
  server.on("/holiday", HTTP_POST, guarded([] {
    int id = server.arg("pid").toInt();
    if (id > HOL_NONE && id < HOL_COUNT) preview_holiday = id;
    server.sendHeader("Location", "/");
    server.send(303);
  }));
  server.on("/tag/reset", HTTP_POST, guarded([] { tag_action_req(TAG_RESET, "resetting tag..."); }));
  server.on("/tag/swstest", HTTP_POST, guarded([] { tag_action_req(TAG_SWS_TEST, "testing SWS link..."); }));
  server.on("/debug/wifiscreen", HTTP_POST, guarded([] { tag_action_req(TAG_DEMO_WIFI, "showing wifi screen..."); }));
  server.on("/debug/serverscreen", HTTP_POST, guarded([] { tag_action_req(TAG_DEMO_SERVER, "showing server screen..."); }));
  server.on("/debug/reboot", HTTP_POST, guarded([] { server.send(200, "text/plain", "rebooting"); delay(300); ESP.restart(); }));
  server.begin();
  app_lock();
  String host = settings.hostname;
  app_unlock();
  start_mdns(host);
}

void web_loop() {
  static bool was_up = true;
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !was_up) start_mdns(mdns_name);  // re-announce weather.local after a Wi-Fi outage
  was_up = up;
  server.handleClient();
}
