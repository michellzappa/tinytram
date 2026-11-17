// tinytram: next trams at one Amsterdam stop, on the Waveshare
// ESP32-S3-Touch-AMOLED-1.75 round board. Data: OVapi (open, no key).
//
// The Arduino loop draws; net.cpp fetches on its own task. Tap the glass to
// fetch now.
#include <Arduino.h>
#include <Preferences.h>
#include <sys/time.h>
#include <time.h>

#include "board_display.h"
#include "board_input.h"
#include "config.h"
#include "departures.h"
#include "face.h"

static const uint32_t FRAME_MS = 200;
static Preferences prefs;

// Mode and zoom survive a power cut.
static void saveView() {
  prefs.putBool("table", faceTableMode());
  prefs.putUChar("zoom", faceZoom());
}
static uint8_t appliedBrightness = 0;

static void applyBrightness(uint8_t b) {
  if (b == appliedBrightness) return;
  appliedBrightness = b;
  boardSetBrightness(b);
}

static uint8_t brightnessFor(time_t now) {
  if (now < 1700000000) return BRIGHT_DAY;
  struct tm t;
  localtime_r(&now, &t);
  const bool night = NIGHT_FROM_H > NIGHT_TO_H
                         ? (t.tm_hour >= NIGHT_FROM_H || t.tm_hour < NIGHT_TO_H)
                         : (t.tm_hour >= NIGHT_FROM_H && t.tm_hour < NIGHT_TO_H);
  return night ? BRIGHT_NIGHT : BRIGHT_DAY;
}

// Eight-step orbit, two minutes a step, at most 3 px from center.
static void burnInShift(time_t now) {
  static const int8_t orbit[8][2] = {{0, 0}, {2, 1}, {3, -1}, {1, -3},
                                     {-1, -2}, {-3, 0}, {-2, 2}, {0, 3}};
  const uint8_t i = (uint8_t)((now / 120) % 8);
  faceSetShift(orbit[i][0], orbit[i][1]);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== tinytram ===");
  if (!boardDisplayBegin()) Serial.println("display init failed");
  boardInputBegin();
  applyBrightness(BRIGHT_DAY);
  gfx->clear(0);
  gfx->flush();
  prefs.begin("tinytram", false);
  faceSetMode(prefs.getBool("table", false), prefs.getUChar("zoom", 0));
  netBegin();
}

void loop() {
  InputFrame in;
  boardInputPoll(&in);
  // Tap the glass: fetch now. BOOT: map zoom. PWR: map or table.
  if (in.tap) {
    Serial.println("tap: refresh");
    netRefreshNow();
  }
  if (in.aPressed) {
    if (faceTableMode()) faceToggleTable();   // BOOT in table mode goes back to the map
    else faceToggleZoom();
    Serial.printf("boot: %s zoom %u\n", faceTableMode() ? "table" : "map", faceZoom());
    saveView();
  }
  if (in.bPressed) {
    faceToggleTable();
    Serial.printf("pwr: %s\n", faceTableMode() ? "table" : "map");
    saveView();
  }

  static uint32_t lastFrame = 0;
  static uint32_t wakeUntil = 0;
  const uint32_t ms = millis();
  if (in.touchBegan || in.aPressed || in.bPressed) wakeUntil = ms + 15000;
  if (ms - lastFrame < FRAME_MS) {
    delay(5);
    return;
  }
  lastFrame = ms;

  struct timeval tv;
  gettimeofday(&tv, nullptr);
  const double nowF = tv.tv_sec + tv.tv_usec / 1e6;

  // At night the face dims; a touch brings it up for fifteen seconds.
  applyBrightness((int32_t)(wakeUntil - ms) > 0 ? BRIGHT_DAY : brightnessFor(tv.tv_sec));
  burnInShift(tv.tv_sec);

  static Board snap;
  netSnapshot(&snap);
  faceDraw(snap, nowF, ms);
  gfx->flush();
}
