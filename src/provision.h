#pragma once

// Wi-Fi setup without recompiling. The device opens its own hotspot with a
// small web page; the networks people save there live in NVS. secrets.h, when
// present, adds networks at build time.
#include <Arduino.h>

static const uint8_t MAX_SAVED = 4;

struct SavedNet {
  char ssid[33];
  char pass[65];
};

// Networks saved on the device, newest first. Returns how many.
uint8_t provLoad(SavedNet *out, uint8_t max);
// Puts a network first in the saved list (replacing one with the same name).
void provSave(const SavedNet &n);
// Forgets every saved network.
void provForgetAll();

// Starts the hotspot, the DNS catch-all and the web page. The caller has set
// WIFI_AP_STA. `note` shows on the page (an error from the last attempt).
void provStart(const char *apName, const char *apPass);
void provSetNote(const char *note);
// Serves requests. Returns true when someone submitted a network; it is in *out.
bool provLoop(SavedNet *out);
void provStop();
