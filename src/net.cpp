// Wi-Fi, clock and the OVapi fetch, on their own task so a slow network never
// stalls the face. The face reads a copy of the board under a mutex.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <esp_heap_caps.h>
#include <esp_sntp.h>
#include <string.h>
#include <sys/time.h>

#include <algorithm>

#include "config.h"
#include "departures.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy src/secrets.example.h to src/secrets.h and add your Wi-Fi"
#endif

// ArduinoJson allocator that puts the document in PSRAM.
struct PsramAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override {
    return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
  }
};
static PsramAllocator psramAllocator;

static Board board;
static SemaphoreHandle_t lock;
static WiFiMulti wifiMulti;
static volatile bool refreshAsked = false;
static bool everConnected = false;

void netSnapshot(Board *out) {
  xSemaphoreTake(lock, portMAX_DELAY);
  *out = board;
  xSemaphoreGive(lock);
}

void netRefreshNow() { refreshAsked = true; }

static void setState(NetState s, const char *err = nullptr) {
  xSemaphoreTake(lock, portMAX_DELAY);
  board.state = s;
  if (err) strlcpy(board.lastError, err, sizeof(board.lastError));
  board.rssi = WiFi.status() == WL_CONNECTED ? (int8_t)WiFi.RSSI() : 0;
  xSemaphoreGive(lock);
}

static bool timeValid() { return time(nullptr) > 1700000000; }

// The feed's timestamps are Amsterdam local time without an offset
// ("2026-11-16T13:48:22"); LastUpdateTimeStamp carries one, which is the same
// local offset, so both parse as local. TZ is set, so mktime does the DST.
static time_t parseLocal(const char *s) {
  if (!s) return 0;
  struct tm t = {};
  int Y, M, D, h, m, sec;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  t.tm_year = Y - 1900;
  t.tm_mon = M - 1;
  t.tm_mday = D;
  t.tm_hour = h;
  t.tm_min = m;
  t.tm_sec = sec;
  t.tm_isdst = -1;
  return mktime(&t);
}

// True when `tok` is one comma-separated item of `list`.
static bool listHas(const char *list, const char *tok) {
  const size_t n = strlen(tok);
  for (const char *p = list; *p;) {
    const char *e = strchr(p, ',');
    const size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len == n && strncmp(p, tok, n) == 0) return true;
    if (!e) break;
    p = e + 1;
  }
  return false;
}

static void shortDest(const char *in, char *out, size_t cap) {
  for (const auto &a : DEST_ALIASES) {
    if (strcmp(in, a.from) == 0) {
      strlcpy(out, a.to, cap);
      return;
    }
  }
  strlcpy(out, in, cap);
}

static bool fetchOnce() {
  // One request for both directions.
  String url = "http://v0.ovapi.nl/tpc/";
  for (uint8_t d = 0; d < NUM_DIRECTIONS; d++) {
    if (d) url += ",";
    url += DIRECTIONS[d].tpcs;
  }

  HTTPClient http;
  http.setTimeout(10000);
  http.setUserAgent("tinytram/0.1 (ESP32 departure display)");
  if (!http.begin(url)) {
    setState(NET_FETCH_FAILED, "bad url");
    return false;
  }
  const int code = http.GET();
  if (code != 200) {
    char e[32];
    snprintf(e, sizeof(e), "http %d", code);
    http.end();
    setState(NET_FETCH_FAILED, e);
    Serial.printf("fetch: %s\n", e);
    return false;
  }

  // The response is ~60 KB and every key is a journey id, so a filter cannot
  // name them (ArduinoJson has no wildcard for object keys). Parse it whole,
  // into PSRAM, so internal RAM stays free for Wi-Fi.
  // OVapi answers chunked even to HTTP/1.0, so the raw stream is not plain
  // JSON. getString() removes the chunk framing; a 60 KB String lands in PSRAM.
  const String body = http.getString();
  JsonDocument doc(&psramAllocator);
  const DeserializationError err = deserializeJson(doc, body);
  http.end();
  if (err) {
    setState(NET_FETCH_FAILED, err.c_str());
    Serial.printf("fetch: json %s\n", err.c_str());
    return false;
  }

  const time_t now = time(nullptr);
  static Departure tmp[128];
  uint8_t n = 0;
  for (JsonPair tp : doc.as<JsonObject>()) {
    const char *tpc = tp.key().c_str();
    int8_t dir = -1;
    for (uint8_t d = 0; d < NUM_DIRECTIONS; d++)
      if (listHas(DIRECTIONS[d].tpcs, tpc)) dir = (int8_t)d;
    if (dir < 0) continue;

    for (JsonPair pp : tp.value()["Passes"].as<JsonObject>()) {
      if (n >= 128) break;
      JsonObject p = pp.value().as<JsonObject>();
      const char *line = p["LinePublicNumber"] | "";
      const char *status = p["TripStopStatus"] | "";
      if (strlen(LINE_FILTER) && !listHas(LINE_FILTER, line)) continue;
      if (strcmp(status, "PASSED") == 0) continue;

      const time_t exp = parseLocal(p["ExpectedDepartureTime"] | "");
      const time_t tgt = parseLocal(p["TargetDepartureTime"] | "");
      const time_t upd = parseLocal(p["LastUpdateTimeStamp"] | "");
      if (!exp || exp < now - 30) continue;

      Departure &d = tmp[n++];
      strlcpy(d.line, line, sizeof(d.line));
      d.journey = p["JourneyNumber"] | 0u;
      shortDest(p["DestinationName50"] | "", d.dest, sizeof(d.dest));
      d.when = exp;
      d.delayS = tgt ? (int16_t)constrain((long)(exp - tgt), -3600L, 3600L) : 0;
      d.dir = (uint8_t)dir;
      d.cancelled = strcmp(status, "CANCEL") == 0;
      // "Live" means a vehicle position fed this prediction. The feed has no
      // single flag for it: a recent update, or a prediction that differs
      // from the timetable, are the two signs. Timetable-only passes carry
      // last night's planning timestamp and the planned time unchanged.
      const bool driving = strcmp(status, "DRIVING") == 0 ||
                           strcmp(status, "ARRIVED") == 0;
      d.live = !d.cancelled && driving &&
               ((upd && now - upd < 900) || (tgt && exp != tgt));
    }
  }
  std::sort(tmp, tmp + n,
            [](const Departure &a, const Departure &b) { return a.when < b.when; });
  if (n > MAX_DEPS) n = MAX_DEPS;

  xSemaphoreTake(lock, portMAX_DELAY);
  memcpy(board.deps, tmp, sizeof(Departure) * n);
  board.count = n;
  board.fetchedAt = now;
  board.fetchedMs = millis();
  board.state = NET_OK;
  board.lastError[0] = 0;
  board.rssi = (int8_t)WiFi.RSSI();
  xSemaphoreGive(lock);
  Serial.printf("fetch: %u departures, heap %u, rssi %d\n", n,
                (unsigned)ESP.getFreeHeap(), (int)WiFi.RSSI());
  return true;
}

// Live positions from the Worker. Small (< 2 KB), so a plain parse.
static bool fetchPositions() {
  String url = POSITIONS_URL;
  url += "?lat=" STOP_LAT "&lon=" STOP_LON "&r=" POSITIONS_RADIUS_M
         "&op=" OPERATOR "&lines=" POSITION_LINES;
  HTTPClient http;
  http.setTimeout(8000);
  http.setUserAgent("tinytram/0.1");
  if (!http.begin(url)) return false;
  const int code = http.GET();
  if (code != 200) {
    Serial.printf("positions: http %d\n", code);
    http.end();
    return false;
  }
  const String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    Serial.println("positions: bad json");
    return false;
  }
  static Vehicle tmp[MAX_VEHICLES];
  uint8_t n = 0;
  for (JsonObject v : doc["v"].as<JsonArray>()) {
    if (n >= MAX_VEHICLES) break;
    const float la = v["la"] | 0.0f, lo = v["lo"] | 0.0f;
    if (la == 0.0f || lo == 0.0f) continue;
    // The feed names lines by planning number; the face uses public numbers.
    const char *plan = v["l"] | "";
    const char *pub = nullptr;
    for (const auto &li : LINES)
      if (strcmp(li.plan, plan) == 0) pub = li.pub;
    if (!pub) continue;
    Vehicle &o = tmp[n++];
    strlcpy(o.line, pub, sizeof(o.line));
    o.journey = v["j"] | 0u;
    o.lat = la;
    o.lon = lo;
    o.age = (int16_t)constrain((long)(v["a"] | -1), -1L, 30000L);
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  memcpy(board.vehicles, tmp, sizeof(Vehicle) * n);
  board.vehicleCount = n;
  board.vehiclesMs = millis();
  board.vehiclesSeq++;
  xSemaphoreGive(lock);
  Serial.printf("positions: %u trams\n", n);
  return true;
}

static void setFetching(bool f) {
  xSemaphoreTake(lock, portMAX_DELAY);
  board.fetching = f;
  board.lastTryMs = millis();
  xSemaphoreGive(lock);
}

static void netTask(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("tinytram");
  for (const auto &c : WIFI_NETWORKS) wifiMulti.addAP(c.ssid, c.pass);
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");

  uint32_t nextFetch = 0;
  uint32_t nextPositions = 0;
  for (;;) {
    if (wifiMulti.run(8000) != WL_CONNECTED) {
      setState(everConnected ? NET_OFFLINE : NET_CONNECTING);
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    if (!everConnected) {
      everConnected = true;
      Serial.printf("wifi: %s %s\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
    }
    if (!timeValid()) {
      setState(NET_NO_TIME);
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    const uint32_t ms = millis();
    if (refreshAsked || (int32_t)(ms - nextFetch) >= 0) {
      refreshAsked = false;
      setFetching(true);
      const bool ok = fetchOnce();
      setFetching(false);
      nextFetch = millis() + (ok ? FETCH_EVERY_MS : FETCH_RETRY_MS);
    } else if (HAS_GPS_LINES && (int32_t)(ms - nextPositions) >= 0) {
      const bool ok = fetchPositions();
      nextPositions = millis() + (ok ? POSITIONS_EVERY_MS : FETCH_RETRY_MS);
    } else {
      // Keep the signal reading current between fetches.
      xSemaphoreTake(lock, portMAX_DELAY);
      board.rssi = (int8_t)WiFi.RSSI();
      xSemaphoreGive(lock);
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void netBegin() {
  lock = xSemaphoreCreateMutex();
  // Core 0 next to the Wi-Fi stack; the face runs on the Arduino loop (core 1).
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
}
