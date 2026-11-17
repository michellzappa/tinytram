# tinytram

The trams, buses and metros leaving one Dutch stop, on a small round screen by
the door. It runs on the [Waveshare ESP32-S3-Touch-AMOLED-1.75](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75)
(466x466) over Wi-Fi, and it was built for Vijzelgracht in Amsterdam: tram 1,
7 and 19 on the street and metro 52 underneath.

## What it shows

The default face is a dark map of the streets around the stop, north up. Your
lines run across it in their own colours, side by side where they share
track. Trams and buses sit on the map at their GPS position and glide to each
new fix, with a short trail behind them and a white tip in front, so you can
see which way they are going. A tram on its way to your stop carries its
minutes next to it.

The stop is drawn the way a transit map draws one: a white capsule around the
platforms, with a tick for each direction. A tick lights up in the line
colour when its tram is less than a minute out. A metro station is a small
ring on its line.

The metro has no GPS underground, so the device estimates where each train
is from its live arrival time and draws it as an outline. Solid means a
position from the vehicle; outline means an estimate or a timetable time.

At the bottom are the next departures per direction, and a status line with a
coloured dot: green while the data is fresh, amber when it is a few minutes
old, red when it is stale or the Wi-Fi is gone. The dot pulses during a fetch.
The minutes keep counting down from the clock between fetches, and they turn
grey once the data is too old to trust.

Press PWR for table mode: the same departures as plain rows on black, with
destinations and minutes in straight columns. BOOT switches the map between
700 m and 1.2 km to the edge. A tap on the glass fetches now. The device
remembers the mode and zoom.

It is meant to stay on. The face shifts by a few pixels every two minutes so
the AMOLED does not burn in, and it dims between 23:00 and 07:00 until you
touch it.

## Set it up for your stop

Any stop in the Netherlands should work. Amsterdam is the only one tested so
far; other places are open as issues.

You need Python 3 and [PlatformIO](https://platformio.org/).

```sh
python3 -m venv .venv && .venv/bin/pip install pillow
.venv/bin/python scripts/setup.py --address "Vijzelgracht, Amsterdam" --lines 1,7,19,52
```

`setup.py` finds the platforms near the address, asks OVapi which lines and
destinations use them, checks which of those lines report GPS, and downloads
the streets, water and route shapes around the stop from OpenStreetMap. It
prints what it found, then writes:

- `stops/<stop>.json`, your choices. Edit a label, a colour or a line here and
  run `setup.py --config stops/<stop>.json` again.
- `src/stop.h` and `src/map_data.h`, generated for the firmware.

Leave out `--lines` to keep every line at the stop. `--stop` picks a stop by
name when several are near the address, and `--label 30008072=Leidseplein`
renames a direction. `stops/vijzelgracht.json` is a working example.

Then add your Wi-Fi and flash:

```sh
cp src/secrets.example.h src/secrets.h
pio run -e amoled-175-round -t upload
```

`secrets.h` takes more than one network; the device joins the strongest one it
can see.

## Where the data comes from

All of it is open and needs no account or key.

- Departures come from [OVapi](https://github.com/koch-t/KV78Turbo-OVAPI/wiki),
  every 30 seconds, for the platforms of your stop.
- Positions come from the national GTFS-realtime feed on gtfs.ovapi.nl. That
  file covers every vehicle in the country and is about 570 KB, too much for a
  microcontroller every 15 seconds. A small Cloudflare Worker in `worker/`
  fetches it, keeps your operator's lines near your stop, and hands the device
  a few hundred bytes of JSON.
- Platform coordinates come from the national GTFS stop list. The setup
  script reads only `stops.txt` out of the 160 MB archive.
- The map is OpenStreetMap data, © OpenStreetMap contributors (ODbL), drawn
  once on your computer and stored in the firmware.

The firmware points at a public instance of the Worker. If you fork the
project or run several devices, deploy your own (`cd worker && pnpm install &&
pnpm exec wrangler deploy`) and change `POSITIONS_URL` in `src/config.h`.

Some operators do not publish GPS for every line. Setup checks each line and
marks those without positions, and the device then estimates them the same
way it does the metro.

## Hardware

The Waveshare ESP32-S3-Touch-AMOLED-1.75: ESP32-S3 with 8 MB PSRAM and 16 MB
flash, a 466x466 CO5300 AMOLED, CST9217 touch and an AXP2101 power chip. The
display, touch and power code comes from
[tinyarcana](https://github.com/michellzappa/tinyarcana), which runs on the
same board.

## License

MIT. Inter is under the SIL Open Font License, and generated maps carry
OpenStreetMap data under the ODbL. See `LICENSE` for the details.
