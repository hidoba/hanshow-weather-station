#pragma once
// State shared between the display worker task and the web server (loop task).
#include <Arduino.h>
#include "settings.h"
#include "tag.h"
#include "weather.h"

struct Status {
  bool have_weather = false;
  String last_error;
  uint32_t last_fetch = 0, last_full = 0, last_fast = 0;  // millis(), 0 = never
  bool tag_ok = false;
  int led = 0;                // tag LED brightness, 0.1 % units
  String tag_info;            // last ping reply (firmware version, bank)
  String fw_status;           // tag firmware update progress / result
  String settings_msg;        // problem with the last settings save (kept until the next save)
  Weather wx;
};

extern Settings settings;     // guarded by app_lock()
extern Status status;         // guarded by app_lock()
extern uint8_t preview[2][EpdCanvas::PLANE];  // last image pushed to the tag
extern uint32_t preview_version;              // bumped whenever preview changes
extern volatile bool request_full;            // set by the web page
extern uint8_t *volatile tag_fw;              // pending tag firmware image (heap), set by the web page
extern volatile size_t tag_fw_len;
extern volatile bool tag_fw_sws;
extern volatile int preview_holiday;           // HolidayId to show for a minute (-1 = none), set by the web page              // flash via SWS (recovery) instead of the UART link
enum TagAction { TAG_NONE, TAG_RESET, TAG_SWS_TEST, TAG_DEMO_WIFI, TAG_DEMO_SERVER };
extern volatile int tag_action;               // set by the web page

// Show a message on the display (black/white fast refresh) from any task; the worker draws it and
// keeps it for hold_s seconds before the weather comes back.
void post_message(std::initializer_list<String> lines, uint32_t hold_s);
bool message_shown();  // the last posted message has been drawn (or failed to)

void app_lock();
void app_unlock();
void web_begin();
void web_loop();
