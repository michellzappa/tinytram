// Copy to secrets.h (gitignored) and fill in. Several networks are fine: the
// device joins the strongest one it can see.
#pragma once

struct WifiCred {
  const char *ssid;
  const char *pass;
};

static const WifiCred WIFI_NETWORKS[] = {
    {"your-ssid", "your-password"},
};
