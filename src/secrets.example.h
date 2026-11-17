// Optional. Wi-Fi is normally set up on the device (hold BOOT). To build
// networks in, copy to secrets.h (gitignored) and fill in. Several are fine: the
// device joins the strongest one it can see.
#pragma once

struct WifiCred {
  const char *ssid;
  const char *pass;
};

static const WifiCred WIFI_NETWORKS[] = {
    {"your-ssid", "your-password"},
};
