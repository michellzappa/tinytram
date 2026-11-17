// Departure board state: what the network task fetched, what the face draws.
#pragma once

#include <Arduino.h>
#include <time.h>

static const uint8_t MAX_DEPS = 40;

struct Departure {
  char line[4];
  uint32_t journey;   // OVapi JourneyNumber; links to a Vehicle
  char dest[28];
  time_t when;        // expected departure, epoch seconds
  int16_t delayS;     // expected minus planned, seconds
  uint8_t dir;        // index into DIRECTIONS
  bool live;          // the vehicle reported in recently (not just timetable)
  bool cancelled;
};

static const uint8_t MAX_VEHICLES = 32;

// One tram's GPS position, from the tinytram-positions Worker.
struct Vehicle {
  char line[4];
  uint32_t journey;
  float lat, lon;
  int16_t age;        // seconds old when the Worker answered, -1 unknown
};

enum NetState : uint8_t {
  NET_CONNECTING = 0,  // no Wi-Fi yet
  NET_NO_TIME,         // Wi-Fi up, waiting for NTP
  NET_OK,              // last fetch worked
  NET_FETCH_FAILED,    // Wi-Fi up, last fetch failed
  NET_OFFLINE,         // Wi-Fi lost after having been up
  NET_SETUP,           // own hotspot open for Wi-Fi setup
};

struct Board {
  Departure deps[MAX_DEPS];   // sorted by `when`
  uint8_t count = 0;
  time_t fetchedAt = 0;       // epoch of the last good fetch, 0 = never
  uint32_t fetchedMs = 0;     // millis() of the last good fetch
  uint32_t lastTryMs = 0;     // millis() of the last attempt, good or bad
  Vehicle vehicles[MAX_VEHICLES];
  uint8_t vehicleCount = 0;
  uint32_t vehiclesMs = 0;    // millis() of the last good position fetch, 0 = never
  uint32_t vehiclesSeq = 0;   // increments on every good position fetch
  NetState state = NET_CONNECTING;
  int8_t rssi = 0;            // dBm, 0 = unknown
  bool fetching = false;
  char lastError[32] = "";
  // Wi-Fi setup: the hotspot to join, its password, and the last problem.
  char setupAp[24] = "";
  char setupPass[12] = "";
  char setupNote[48] = "";
};

// Starts the network task. It owns Wi-Fi, NTP and the fetch loop.
void netBegin();
// Copies the current board under the lock. The face never reads it live.
void netSnapshot(Board *out);
// Asks for a fetch now (a tap on the glass).
void netRefreshNow();
// Opens Wi-Fi setup, or closes it when it is open (BOOT held).
void netToggleSetup();
