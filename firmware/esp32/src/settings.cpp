#include "settings.h"
#include "config.h"
#include <Preferences.h>

void settings_load(Settings &s) {
  Preferences p;
  p.begin("wx", true);
  s.city = p.getString("city", DEFAULT_CITY);
  s.place = p.getString("place", "");
  s.lat = p.getFloat("lat", NAN);
  s.lon = p.getFloat("lon", NAN);
  s.weather_minutes = p.getInt("wmin", DEFAULT_WEATHER_MINUTES);
  s.clock = p.getBool("clock", DEFAULT_CLOCK);
  s.fahrenheit = p.getBool("fahr", DEFAULT_FAHRENHEIT);
  s.holidays = p.getBool("holidays", DEFAULT_HOLIDAYS);
  String b = p.getString("bdays", "");  // "name|month|day\n..."
  for (auto &bd : s.birthdays) bd = Birthday{"", 0, 0};
  for (int i = 0, pos = 0; i < MAX_BIRTHDAYS && pos < (int)b.length(); i++) {
    int end = b.indexOf('\n', pos);
    if (end < 0) end = b.length();
    String line = b.substring(pos, end);
    int p1 = line.indexOf('|'), p2 = line.lastIndexOf('|');
    if (p1 > 0 && p2 > p1)
      s.birthdays[i] = Birthday{line.substring(0, p1), (uint8_t)line.substring(p1 + 1, p2).toInt(), (uint8_t)line.substring(p2 + 1).toInt()};
    pos = end + 1;
  }
  s.hostname = p.getString("host", DEFAULT_HOSTNAME);
  p.end();
}

void settings_save(const Settings &s) {
  Preferences p;
  p.begin("wx", false);
  p.putString("city", s.city);
  p.putString("place", s.place);
  p.putFloat("lat", s.lat);
  p.putFloat("lon", s.lon);
  p.putInt("wmin", s.weather_minutes);
  p.putBool("clock", s.clock);
  p.putBool("fahr", s.fahrenheit);
  p.putBool("holidays", s.holidays);
  String b;
  for (auto &bd : s.birthdays)
    if (bd.name.length() && bd.month && bd.day) b += bd.name + "|" + bd.month + "|" + bd.day + "\n";
  p.putString("bdays", b);
  p.putString("host", s.hostname);
  p.end();
}
