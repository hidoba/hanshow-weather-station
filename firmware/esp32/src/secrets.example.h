#pragma once
// Your settings for the first flash: copy this file to secrets.h and edit the three values.
// secrets.h is git-ignored.

// Wi-Fi network the weather station joins.
#define WIFI_SSID "your-wifi"
#define WIFI_PASS "your-wifi-password"

// Password for the settings page (user name "admin") and for over-the-air updates,
// including `pio run -e ota -t upload`. Change it! Set to "" to disable the protection.
#define ADMIN_PASSWORD "admin"
