# tinytram: agent notes

Live departures and a live map for one Dutch public transport stop, on the
Waveshare ESP32-S3-Touch-AMOLED-1.75 round board (466x466 CO5300, CST9217
touch, AXP2101). Desk-specific notes, if any, are in `AGENTS.local.md`
(gitignored).

## Layout

- `scripts/setup.py`: address or coordinates -> `stops/<stop>.json` (the
  choices, committed) -> `src/stop.h` and `src/map_data.h` (generated,
  gitignored). The firmware does not build without them.
- `src/`: firmware. `config.h` is general settings; `stop.h` is the stop.
  `net.cpp` fetches (own FreeRTOS task), `face.cpp` draws, `main.cpp` loops.
- `worker/`: Cloudflare Worker that turns the national GTFS-realtime vehicle
  positions (570 KB protobuf) into a few hundred bytes of JSON per stop.
- `scripts/build_fonts.py`: Inter -> `src/fonts/*.h`.

## Data sources and their traps

- **Platforms:** GTFS `stops.txt`, read with HTTP range requests out of the
  160 MB `gtfs-nl.zip`. `stop_ifopt` is `NL:Q:<timing point code>`; that code
  is OVapi's TPC and the coordinates are the platform's.
- **Departures:** OVapi `/tpc/<a>,<b>`. Times are Amsterdam local without an
  offset. The body is chunked even to HTTP/1.0, so `net.cpp` uses
  `getString()`. ArduinoJson filters have no wildcard for object keys, and
  every key is an id, so the whole body is parsed into PSRAM.
- **Positions:** GTFS-rt entity ids are `date:OPERATOR:line:journey`. `line`
  is the planning number (LINES maps it to the public number); `journey`
  equals OVapi `JourneyNumber`, which links a dot to its departure. A line can
  have entities without coordinates (Amsterdam metro), so setup checks for an
  actual position before it marks a line as GPS.
- **Lines without GPS** are placed on their direction's path from the live
  arrival time at `estMps` (`drawEstimated`), and always draw as outlines.
- **Map:** OpenStreetMap via Overpass (ODbL). Overpass is often at capacity;
  setup retries and caches in `.cache/`.

## Build and flash

    python3 -m venv .venv && .venv/bin/pip install pillow
    .venv/bin/python scripts/setup.py --config stops/vijzelgracht.json
    cp src/secrets.example.h src/secrets.h      # optional: Wi-Fi built in
    pio run -e amoled-175-round -t upload

Read the board's MAC before flashing if several identical boards are around:
a wrong pin map looks like dead hardware. The board layer
(`board_display.*`, `board_input.*`, `pin_config.h`) is copied from
tinyarcana; do not change a pin without a source.

## Face layers (`faceDraw`, in order)

map -> station capsule and rings -> estimated vehicles -> GPS vehicles ->
dark bands -> text. Table mode skips the map and vehicles.

## Controls

Tap: fetch now. BOOT: map zoom (in table mode, back to the map). BOOT held:
Wi-Fi setup hotspot (provision.cpp, networks in NVS "wifi"). PWR: map or
table. Mode and zoom persist in NVS (`tinytram`).
