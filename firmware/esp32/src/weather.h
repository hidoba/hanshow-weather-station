#pragma once
#include <Arduino.h>

struct Weather {
  // current
  float temp, feels, wind;
  int humidity, code;
  bool is_day;
  int hour, minute;                     // local time of the observation (15 min steps)
  int utc_offset;                       // seconds, for the clock
  bool fahrenheit;                      // temperatures in F, wind in mph
  // hourly temperature, 49 points: now-24h .. now+24h (index 24 = current hour)
  static const int CURVE = 49;
  float curve[CURVE];
  int curve_hour0;                      // local hour-of-day of curve[0]
  int precip[CURVE];                    // precipitation probability %
  // daily: 0 = yesterday, 1 = today, 2.. = forecast
  static const int DAYS = 4;
  float tmax[DAYS], tmin[DAYS], rain[DAYS];
  int dcode[DAYS], wday[DAYS];          // wday 0 = Sunday
  int sunrise_min = 7 * 60, sunset_min = 19 * 60;  // today, local minutes since midnight
  // air quality (NAN / -1 if unavailable)
  float pm25 = NAN;
  int eaqi = -1;                        // European AQI
};

struct Place {
  String name;
  float lat, lon;
};

bool geocode(const String &query, Place &out);  // Open-Meteo geocoding
bool fetch_weather(float lat, float lon, bool fahrenheit, Weather &w);
bool fetch_air(float lat, float lon, Weather &w);
