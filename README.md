# tinytram

Amsterdam's trams and metro, live on a small round screen by the door. It
draws the city around your stop as a dark map, puts every tram on it where it
is right now, and lists the next departures each way underneath. It runs on
the [Waveshare ESP32-S3-Touch-AMOLED-1.75](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75)
(466x466) over Wi-Fi.

Pick any stop in Amsterdam with one setup command. The example in this repo is
Vijzelgracht, with tram 1, 7 and 19 on the street and metro 52 underneath.

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
is from its live arrival time and draws it the same way as a tram. A tram
whose last GPS fix is old turns into an outline.

At the bottom are the next departures per direction, and a status line with a
coloured dot: green while the data is fresh, amber when it is a few minutes
old, red when it is stale or the Wi-Fi is gone. The dot pulses during a fetch.
The minutes keep counting down from the clock between fetches, and they turn
grey once the data is too old to trust.

Press PWR for table mode: the same departures as plain rows on black, with
destinations and minutes in straight columns. BOOT switches the map between
700 m and 1.2 km to the edge; hold BOOT for Wi-Fi setup. A tap on the glass
fetches now. The device
remembers the mode and zoom.

It is meant to stay on. The face shifts by a few pixels every two minutes so
the AMOLED does not burn in, and it dims between 23:00 and 07:00 until you
touch it.

## Set it up

Any stop in Amsterdam works. Other Dutch cities use the same data and should
work too, but nobody has tested them yet; see the open issues.

You need Python 3, git and a USB-C cable.

1. Get the code and the tools:

   ```sh
   git clone https://github.com/michellzappa/tinytram && cd tinytram
   python3 -m venv .venv && .venv/bin/pip install pillow platformio
   ```

2. Choose your stop. Give an address near it:

   ```sh
   .venv/bin/python scripts/setup.py --address "Museumplein, Amsterdam"
   ```

   The script takes the stop nearest the address, prints its lines and
   directions, and names the other stops close by. To use one of those, run it
   again with `--stop "<name>"`. Add `--lines 2,12` to keep only some lines;
   without it, setup keeps every line. To try the example first, run
   `.venv/bin/python scripts/setup.py --config stops/vijzelgracht.json`.

3. Plug in the board and flash it:

   ```sh
   .venv/bin/pio run -e amoled-175-round -t upload
   ```

   If the upload cannot connect, unplug the board, hold BOOT while you plug it
   back in, and run the command again.

4. Set up Wi-Fi from your phone. On first start the board shows "Wi-Fi
   setup", a hotspot name like `tinytram-55AC` and an 8-digit password. Join
   that hotspot on your phone. A page opens; if it does not, open
   `192.168.4.1` in a browser. Choose your network, type its password and tap
   Join. The board joins it, remembers it, and shows the map. Departures
   appear within half a minute.

   The board only sees 2.4 GHz networks. If your router shows two names, pick
   the 2.4 GHz one.

To change networks later, for example after a move, hold BOOT for a second.
The setup screen opens; hold BOOT again to leave it. The board remembers up
to four networks and joins whichever one is in range. If it cannot join any
of them for a minute after it starts, it opens setup by itself, and goes back
to trying its saved networks after five minutes (a router that comes back
slowly after a power cut).

You can also build Wi-Fi in: copy `src/secrets.example.h` to `src/secrets.h`
and fill it in before you flash. `secrets.h` is gitignored. This suits a board
you set up for someone else's network.

You do not need a server. The firmware uses a public instance of the
positions Worker (see below) out of the box.

### What setup writes

- `stops/<stop>.json` holds your choices. Edit a label, a colour or a line
  there, then run `setup.py --config stops/<stop>.json` again.
- `src/stop.h` and `src/map_data.h` are generated for the firmware. The build
  stops with a clear error until they exist.
- `--label 30008072=Leidseplein` renames a direction. The number is the
  platform code that setup prints.

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

### Run your own positions Worker (optional)

The firmware points at a public instance of the Worker. It is free to run your
own, and worth it if you fork the project or run several devices. You need
Node.js, [pnpm](https://pnpm.io/) and a free Cloudflare account.

```sh
cd worker && pnpm install
pnpm exec wrangler login
pnpm exec wrangler deploy
```

`wrangler login` opens a browser to sign in to Cloudflare. `deploy` prints
the Worker's address, for example
`https://tinytram-positions.<your-subdomain>.workers.dev`. Put that address
in `POSITIONS_URL` in `src/config.h` with `http://` instead of `https://` and
`/v` at the end, then flash again. The device uses plain HTTP to save memory;
the data is public timetable data. Open the address in a browser to check it:
the root path answers with one line of text.

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
OpenStreetMap data under the ODbL. See `THIRD_PARTY.md` for the details.
