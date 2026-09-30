#pragma once
#include "tag.h"
#include "weather.h"
#include "holidays.h"

struct Clock {
  bool show;            // draw the current time
  int hour, minute;     // current local time
  int upd_hour, upd_min;  // when the weather was fetched
  int year, month, day;   // current local date (0 if unknown)
  bool stale;             // several updates in a row failed
};

// hol: on a holiday its illustration replaces the temperature curve (nullptr / HOL_NONE = curve).
void render(EpdCanvas &g, const Weather &w, const String &place, const Clock &clk, const Holiday *hol = nullptr);
void render_icon_gallery(EpdCanvas &g);  // debug

// Black/white status screens (shown with the fast refresh): title bar, cloud art, text lines.
void render_status(EpdCanvas &g, const String lines[], int n);
