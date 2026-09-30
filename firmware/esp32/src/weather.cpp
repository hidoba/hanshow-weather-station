#include "log.h"
#include "weather.h"
#include "config.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

String air_status = "-";
static String last_http_error;

static bool get_json(const String &url, JsonDocument &doc) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  int rc = http.GET();
  if (rc != 200) { logf("http %d: %s\n", rc, url.c_str()); last_http_error = "http " + String(rc); http.end(); return false; }
  DeserializationError e = deserializeJson(doc, http.getString());
  http.end();
  if (e) { logf("json %s: %s\n", e.c_str(), url.c_str()); last_http_error = String("json ") + e.c_str(); return false; }
  return true;
}

static String urlencode(const String &s) {
  String o;
  for (char c : s) {
    if (isalnum((uint8_t)c) || c == '-' || c == '_' || c == '.') o += c;
    else { char b[4]; snprintf(b, sizeof b, "%%%02X", (uint8_t)c); o += b; }
  }
  return o;
}

bool geocode(const String &query, Place &out) {
  JsonDocument doc;
  if (!get_json("https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&format=json&name=" + urlencode(query), doc))
    return false;
  JsonObject r = doc["results"][0];
  if (r.isNull()) return false;
  out.name = r["name"].as<String>();
  out.lat = r["latitude"];
  out.lon = r["longitude"];
  logf("geocode: %s -> %s (%s) %.4f,%.4f\n", query.c_str(), out.name.c_str(),
                r["country"] | "", out.lat, out.lon);
  return true;
}

bool fetch_air(float lat, float lon, Weather &w) {
  char url[256];
  snprintf(url, sizeof url,
           "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.4f&longitude=%.4f"
           "&current=pm2_5,european_aqi",
           lat, lon);
  JsonDocument doc;
  w.pm25 = NAN;
  w.eaqi = -1;
  if (!get_json(url, doc)) { air_status = last_http_error; return false; }
  air_status = "ok";
  if (!doc["current"]["pm2_5"].isNull()) w.pm25 = doc["current"]["pm2_5"];
  w.eaqi = doc["current"]["european_aqi"] | -1;
  logf("air: pm2.5 %.1f eaqi %d\n", w.pm25, w.eaqi);
  return true;
}

static int weekday(const char *iso) {  // "YYYY-MM-DD" -> 0 = Sunday
  int y, m, d;
  if (sscanf(iso, "%d-%d-%d", &y, &m, &d) != 3) return 0;
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y--;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

bool fetch_weather(float lat, float lon, bool fahrenheit, Weather &w) {
  char url[512];
  snprintf(url, sizeof url,
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,weather_code,wind_speed_10m"
           "&hourly=temperature_2m,precipitation_probability"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum,sunrise,sunset"
           "&timezone=auto&past_days=1&forecast_days=3%s",
           lat, lon, fahrenheit ? "&temperature_unit=fahrenheit&wind_speed_unit=mph" : "");
  w.fahrenheit = fahrenheit;
  JsonDocument doc;
  if (!get_json(url, doc)) return false;

  JsonObject cur = doc["current"];
  w.temp = cur["temperature_2m"];
  w.feels = cur["apparent_temperature"];
  w.humidity = cur["relative_humidity_2m"];
  w.wind = cur["wind_speed_10m"];
  w.code = cur["weather_code"];
  w.is_day = cur["is_day"].as<int>() == 1;
  const char *t = cur["time"] | "";
  if (sscanf(t, "%*d-%*d-%*dT%d:%d", &w.hour, &w.minute) != 2) return false;
  w.utc_offset = doc["utc_offset_seconds"] | 0;

  // hourly[] starts at yesterday 00:00 local
  JsonArray ht = doc["hourly"]["temperature_2m"], hp = doc["hourly"]["precipitation_probability"];
  int now = 24 + w.hour, first = now - 24;
  if ((int)ht.size() < first + Weather::CURVE) return false;
  w.curve_hour0 = w.hour;
  for (int i = 0; i < Weather::CURVE; i++) {
    w.curve[i] = ht[first + i].isNull() ? NAN : ht[first + i].as<float>();
    w.precip[i] = hp[first + i] | 0;
  }

  JsonObject d = doc["daily"];
  if ((int)d["time"].size() < Weather::DAYS) return false;
  for (int i = 0; i < Weather::DAYS; i++) {
    w.tmax[i] = d["temperature_2m_max"][i];
    w.tmin[i] = d["temperature_2m_min"][i];
    w.rain[i] = d["precipitation_sum"][i];
    w.dcode[i] = d["weather_code"][i];
    w.wday[i] = weekday(d["time"][i] | "");
  }
  int h, m;
  if (sscanf(d["sunrise"][1] | "", "%*d-%*d-%*dT%d:%d", &h, &m) == 2) w.sunrise_min = h * 60 + m;
  if (sscanf(d["sunset"][1] | "", "%*d-%*d-%*dT%d:%d", &h, &m) == 2) w.sunset_min = h * 60 + m;
  logf("weather: %.1fC code %d at %02d:%02d\n", w.temp, w.code, w.hour, w.minute);
  return true;
}
