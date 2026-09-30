#pragma once
#include <Arduino.h>

enum HolidayId {
  HOL_NONE,
  HOL_BIRTHDAY,  // from the settings page
  HOL_NEW_YEAR, HOL_VALENTINE, HOL_WOMENS_DAY, HOL_PI_DAY, HOL_ST_PATRICK, HOL_APRIL_FOOLS,
  HOL_SONGKRAN, HOL_HALLOWEEN, HOL_CHRISTMAS, HOL_EASTER, HOL_THANKSGIVING, HOL_LUNAR_NY,
  HOL_MID_AUTUMN, HOL_NOWRUZ, HOL_HIJRI_NY, HOL_EID_FITR, HOL_EID_ADHA, HOL_ROSH_HASHANAH,
  HOL_HANUKKAH, HOL_DIWALI, HOL_HOLI, HOL_SUMMER_SOLSTICE, HOL_WINTER_SOLSTICE,
  HOL_COUNT
};

struct Holiday {
  int id;
  String greeting;  // red, next to the illustration ('\n' = line break, else wrapped)
  String quip;      // a line of fun in black (same rules)
  int arg;          // Hanukkah: night 1..8
};

struct Birthday {
  String name;
  uint8_t month, day;  // 0 = unused
};

struct HolidayContext {
  bool south = false;               // southern hemisphere: the June solstice is the shortest day
  const Birthday *birthdays = nullptr;
  int n_birthdays = 0;
};

// The holiday on a local date (HOL_NONE if none). Birthdays win, then the bigger holidays.
Holiday holiday_for(int year, int month, int day, const HolidayContext &ctx);
// The next occurrence of holiday `id` on or after the given date, as the display will show it on
// the holiday's main day; a fixed sample if the date is unknown (year 0).
Holiday holiday_next(int id, int year, int month, int day, const HolidayContext &ctx);
const char *holiday_name(int id);  // "Halloween"
