#pragma once
// Defaults; everything here except the pins can be changed on the settings web page.
#define FW_VERSION "1.27"

#define DEFAULT_CITY "Hong Kong"
#define DEFAULT_HOSTNAME "weather"     // -> http://weather.local
#define DEFAULT_WEATHER_MINUTES 5      // full (flashing) refresh happens with each weather update
#define DEFAULT_CLOCK true             // per-minute clock via fast refresh
#define DEFAULT_FAHRENHEIT false
#define DEFAULT_POWER_SAVE true      // deep sleep between clock updates (see AWAKE_WINDOW_S)
#define DEFAULT_HOLIDAYS true         // holiday illustrations instead of the curve on special days
#define MAX_BIRTHDAYS 8               // birthdays entered on the settings page
#define RETRY_MINUTES 2                // after a failed fetch

#define TAG_RX_PIN 44                  // <- tag TXD
#define TAG_TX_PIN 43                  // -> tag RXD
#define TAG_NRST_PIN 12                // -> tag NRST (driven high, low = reset)
#define TAG_SWS_PIN 10                 // -> tag SWS (recovery flashing; held high when idle)

// Fast (clock) refresh waveform for red pixels: phase-A LUT byte and length in frames.
// 0xC0 = VSH2 "hold red" top-up; 0x00 = leave red undriven (thin red text fades over time).
#define FAST_RED_DRIVE 0xC0
#define FAST_FRAMES 50

// Tag LED (green blink while refreshing): brightness follows the sun (percent),
// ramping over LED_RAMP_MIN minutes centred on sunrise / sunset.
#define LED_NIGHT_PCT 1.0f
#define LED_DAY_PCT 20.0f
#define LED_RAMP_MIN 60

// Power-on: black/white splash via fast refresh right away; if Wi-Fi is still down after
// WIFI_WARN_S seconds show a "connecting" screen and flash the tag's red LED every 2 s.
#define WIFI_WARN_S 15
// While Wi-Fi is down, restart the connection attempt every WIFI_RETRY_S seconds: the
// library's auto-reconnect can give up for good if the router is gone for a while.
#define WIFI_RETRY_S 30
// Wi-Fi up but the weather server unreachable: status screen (if nothing shown yet) and a red
// double flash every 4 s; retry every FIRST_RETRY_S until the first successful fetch.
#define FIRST_RETRY_S 30

// Onboard RGB LED of the ESP32 board (WS2812; GPIO21 on the Waveshare ESP32-S3-Zero, -1 = none).
// Signals what the tag can't show itself: blue pulses while installing tag firmware over SWS,
// red blinking while the tag doesn't respond.
#define STATUS_LED_PIN 21
// How long an error/info message stays on the display before the weather comes back.
#define MESSAGE_HOLD_S 60

// Power saving: the settings page stays reachable for AWAKE_WINDOW_S after power-on, after the
// BOOT button wakes it, or after the page was last opened. Then the ESP32 deep-sleeps, waking
// once a minute for the clock (Wi-Fi off) and connecting only to fetch the weather.
#define AWAKE_WINDOW_S 300
#define WAKE_BUTTON_PIN 0              // BOOT button on the ESP32-S3-Zero (RTC GPIO, active low)
