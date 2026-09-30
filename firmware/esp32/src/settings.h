#pragma once
#include <Arduino.h>
#include "config.h"
#include "holidays.h"

struct Settings {
  String city;          // as typed by the user
  String place;         // resolved display name
  float lat = NAN, lon = NAN;
  int weather_minutes;
  bool clock;
  bool fahrenheit;       // temperatures in F (and wind in mph)
  bool holidays;          // holiday illustrations on special days
  bool power_save;        // deep sleep when nobody uses the settings page
  Birthday birthdays[MAX_BIRTHDAYS];  // entered on the settings page, stored only on the device
  String hostname;
};

void settings_load(Settings &s);
void settings_save(const Settings &s);
