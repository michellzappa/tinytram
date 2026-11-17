#!/usr/bin/env python3
"""Set tinytram up for any public transport stop in the Netherlands.

    # First time: find the stop near an address (or --lat/--lon), keep some lines
    .venv/bin/python scripts/setup.py --address "Vijzelgracht, Amsterdam" --lines 1,7,19,52

    # Again later, or from a config someone shared
    .venv/bin/python scripts/setup.py --config stops/vijzelgracht.json

It writes:
    stops/<slug>.json   the choices; edit labels or lines here and rerun --config
    src/stop.h          stop, directions, rows, lines, colours (generated)
    src/map_data.h      the background map at two zoom levels, platforms and
                        route paths (generated)

Sources, all open and keyless:
    GTFS stops (gtfs.ovapi.nl)   platform coordinates; stop_ifopt NL:Q:<code>
                                 is the OVapi timing point code. Only stops.txt
                                 is read out of the zip, with HTTP range requests.
    OVapi (v0.ovapi.nl)          which lines and destinations use each platform
    GTFS-rt (gtfs.ovapi.nl)      which lines report GPS positions
    OpenStreetMap via Overpass   streets, water, tracks, route shapes (ODbL)
    Nominatim                    the --address lookup

Needs Pillow:  python3 -m venv .venv && .venv/bin/pip install pillow
"""
import argparse
import csv
import io
import json
import math
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, ".cache")
UA = "tinytram-setup/1.0 (+https://github.com/michellzappa/tinytram)"
GTFS_ZIP = "https://gtfs.ovapi.nl/nl/gtfs-nl.zip"
GTFS_RT = "https://gtfs.ovapi.nl/nl/vehiclePositions.pb"
OVAPI = "http://v0.ovapi.nl"
OVERPASS = ["https://overpass-api.de/api/interpreter",
            "https://overpass.kumi.systems/api/interpreter"]

SIZE = 466
LEVELS = [("NEAR", 700), ("FAR", 1200)]   # glass edge, metres from the stop
SS = 4                                     # supersampling for the map
M_LAT = 111_320.0

# Line colours, handed out in order: surface lines first, then the rest.
PALETTE = [(0x9B, 0x6B, 0xE0), (0xF0, 0xB0, 0x3C), (0x3C, 0xA8, 0xF0),
           (0x2E, 0xC4, 0xB0), (0xE8, 0x6A, 0x3D), (0x3D, 0xC0, 0x80),
           (0xE8, 0x6A, 0xB8), (0xE0, 0xD0, 0x4C)]
# Estimated speed, metres per second including stops, for lines without GPS.
EST_MPS = {"METRO": 10.0, "TRAIN": 20.0, "TRAM": 6.0, "BUS": 6.0, "BOAT": 4.0}
TYPE_NAME = {"METRO": "Metro", "TRAM": "Tram", "BUS": "Bus", "BOAT": "Ferry", "TRAIN": "Train"}
OSM_ROUTE = {"METRO": "subway", "TRAM": "tram", "BUS": "bus", "BOAT": "ferry", "TRAIN": "train"}


def log(*a):
    print(*a, file=sys.stderr)


def http(url, data=None, headers=None, timeout=60):
    h = {"User-Agent": UA}
    h.update(headers or {})
    for attempt in range(5):
        req = urllib.request.Request(url, data=data, headers=h)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code != 429 or attempt == 4:
                raise
            wait = int(e.headers.get("Retry-After") or 0) or 15 * (attempt + 1)
            log("%s: rate limited, waiting %ds" % (urllib.parse.urlparse(url).netloc, wait))
            time.sleep(wait)


# ---------------------------------------------------------------- GTFS stops

class HttpFile(io.RawIOBase):
    """Seekable read-only file over HTTP range requests, so zipfile can pull
    one member out of a large archive."""

    def __init__(self, url):
        self.url = url
        self.pos = 0
        req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": UA})
        with urllib.request.urlopen(req, timeout=60) as r:
            self.size = int(r.headers["Content-Length"])

    def seekable(self):
        return True

    def readable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else self.pos + off if whence == 1 else self.size + off
        return self.pos

    def readinto(self, b):
        n = min(len(b), self.size - self.pos)
        if n <= 0:
            return 0
        data = http(self.url, headers={"Range": "bytes=%d-%d" % (self.pos, self.pos + n - 1)})
        b[:len(data)] = data
        self.pos += len(data)
        return len(data)


def gtfs_platforms():
    """{timing point code: (name, lat, lon)} for every platform in NL."""
    path = os.path.join(CACHE, "platforms.json")
    if os.path.exists(path) and time.time() - os.path.getmtime(path) < 30 * 86400:
        return json.load(open(path))
    log("gtfs: reading stops.txt from %s (about 2 MB of a 160 MB zip)" % GTFS_ZIP)
    z = zipfile.ZipFile(io.BufferedReader(HttpFile(GTFS_ZIP), buffer_size=1 << 20))
    out = {}
    with z.open("stops.txt") as f:
        for r in csv.DictReader(io.TextIOWrapper(f, "utf-8")):
            m = re.match(r"NL:Q:(\d+)$", r.get("stop_ifopt") or "")
            if m:
                out[m.group(1)] = (r["stop_name"], float(r["stop_lat"]), float(r["stop_lon"]))
    os.makedirs(CACHE, exist_ok=True)
    json.dump(out, open(path, "w"))
    log("gtfs: %d platforms" % len(out))
    return out


def metres(lat0, lon0, lat, lon):
    dx = (lon - lon0) * M_LAT * math.cos(math.radians(lat0))
    dy = (lat - lat0) * M_LAT
    return math.hypot(dx, dy)


# ---------------------------------------------------------------- OVapi

def ovapi_tpcs(codes):
    """{tpc: {"lines": {pub: info}, "type": ..., "operator": ...}} from live passes."""
    out = {}
    for i in range(0, len(codes), 40):
        d = json.loads(http(OVAPI + "/tpc/" + ",".join(codes[i:i + 40])))
        for tpc, tp in d.items():
            lines = {}
            types, ops = {}, {}
            for p in tp.get("Passes", {}).values():
                pub = p.get("LinePublicNumber") or ""
                if not pub:
                    continue
                li = lines.setdefault(pub, {"plan": p.get("LinePlanningNumber") or pub,
                                            "type": p.get("TransportType") or "BUS",
                                            "dests": {}})
                dest = p.get("DestinationName50") or ""
                li["dests"][dest] = li["dests"].get(dest, 0) + 1
                types[li["type"]] = types.get(li["type"], 0) + 1
                op = p.get("OperatorCode") or p.get("DataOwnerCode") or ""
                ops[op] = ops.get(op, 0) + 1
            if lines:
                out[tpc] = {"lines": lines,
                            "type": max(types, key=types.get),
                            "operator": max(ops, key=ops.get)}
    return out


# ---------------------------------------------------------------- GTFS-rt

def gps_lines(operator):
    """Planning numbers of this operator's lines whose vehicles report a
    position now. An entity id is "date:OPERATOR:line:journey"; a metro can
    have entities without coordinates, so a position must be present."""
    import struct
    cache = os.path.join(CACHE, "gps-%s.json" % operator)
    if os.path.exists(cache) and time.time() - os.path.getmtime(cache) < 600:
        return set(json.load(open(cache)))
    buf = http(GTFS_RT, timeout=90)

    def varint(j):
        r, sh = 0, 0
        while True:
            b = buf[j]
            j += 1
            r |= (b & 0x7F) << sh
            if b < 0x80:
                return r, j
            sh += 7

    def fields(i, end):
        """Yield (field, wire, value_or_(start, end)) for one message."""
        while i < end:
            key, i = varint(i)
            wire = key & 7
            if wire == 0:
                v, i = varint(i)
                yield key >> 3, wire, v
            elif wire == 2:
                n, i = varint(i)
                yield key >> 3, wire, (i, i + n)
                i += n
            elif wire == 5:
                yield key >> 3, wire, buf[i:i + 4]
                i += 4
            elif wire == 1:
                i += 8
            else:
                return

    lines = set()
    for f, w, v in fields(0, len(buf)):
        if f != 2 or w != 2:
            continue
        eid, has_pos = "", False
        for f2, w2, v2 in fields(*v):
            if f2 == 1 and w2 == 2:
                eid = buf[v2[0]:v2[1]].decode("utf-8", "replace")
            elif f2 == 4 and w2 == 2:                       # vehicle
                for f3, w3, v3 in fields(*v2):
                    if f3 == 2 and w3 == 2:                 # position
                        for f4, w4, v4 in fields(*v3):
                            if f4 == 1 and w4 == 5 and struct.unpack("<f", v4)[0] != 0:
                                has_pos = True
        parts = eid.split(":")
        if has_pos and len(parts) >= 4 and parts[1] == operator:
            lines.add(parts[2])
    os.makedirs(CACHE, exist_ok=True)
    json.dump(sorted(lines), open(cache, "w"))
    return lines


# ---------------------------------------------------------------- Overpass

def overpass(query, cache_name):
    path = os.path.join(CACHE, cache_name)
    if os.path.exists(path):
        return json.load(open(path))
    data = urllib.parse.urlencode({"data": query}).encode()
    for attempt in range(8):
        url = OVERPASS[attempt % len(OVERPASS)]
        try:
            raw = http(url, data=data, timeout=180)
            out = json.loads(raw)
            os.makedirs(CACHE, exist_ok=True)
            json.dump(out, open(path, "w"))
            return out
        except Exception as e:  # noqa: BLE001  Overpass is often at capacity
            wait = 10 * (attempt + 1)
            log("overpass: %s (%s), retry in %ds" % (url, str(e)[:60], wait))
            time.sleep(wait)
    sys.exit("overpass: gave up; try again later")


def osm_base(lat, lon, slug):
    r = 1700
    q = """[out:json][timeout:120];
(
  way["natural"="water"](around:{r},{lat},{lon});
  relation["natural"="water"](around:{r},{lat},{lon});
  way["waterway"~"^(canal|river)$"](around:{r},{lat},{lon});
  way["railway"~"^(tram|rail|light_rail)$"](around:{r},{lat},{lon});
  way["highway"~"^(primary|secondary|tertiary|residential|unclassified|living_street|pedestrian)$"](around:{r},{lat},{lon});
);
out geom qt;""".format(r=r, lat=lat, lon=lon)
    return overpass(q, slug + "-base.json")


def osm_routes(lat, lon, refs_by_type, slug):
    parts = []
    for t, refs in refs_by_type.items():
        rx = "|".join(re.escape(x) for x in sorted(refs))
        parts.append('  relation["route"="%s"]["ref"~"^(%s)$"](around:400,%s,%s);'
                     % (OSM_ROUTE.get(t, "bus"), rx, lat, lon))
    q = "[out:json][timeout:120];\n(\n%s\n);\nout geom qt;" % "\n".join(parts)
    return overpass(q, slug + "-routes-%s.json" % "-".join(
        sorted(r for refs in refs_by_type.values() for r in refs)))


def chain(rel):
    """The route relation as one polyline in travel order: member ways joined
    end to end, each flipped as needed. Gaps are bridged."""
    ways = [[(p["lat"], p["lon"]) for p in m["geometry"] if p]
            for m in rel.get("members", [])
            if m["type"] == "way" and m.get("role") in ("", None, "forward", "backward")
            and m.get("geometry")]
    ways = [w for w in ways if len(w) >= 2]
    if not ways:
        return []
    near = lambda a, b: metres(a[0], a[1], b[0], b[1]) < 15
    path = list(ways[0])
    if len(ways) > 1 and (near(path[0], ways[1][0]) or near(path[0], ways[1][-1])):
        path.reverse()
    for w in ways[1:]:
        if near(path[-1], w[0]) or not near(path[-1], w[-1]) and \
                metres(*path[-1], *w[0]) <= metres(*path[-1], *w[-1]):
            path.extend(w[1:] if near(path[-1], w[0]) else w)
        else:
            path.extend(list(reversed(w))[1:] if near(path[-1], w[-1]) else list(reversed(w)))
    return path


def tokens(s):
    return set(re.findall(r"[a-z]{3,}", (s or "").lower())) - {"amsterdam", "station", "via"}


# ---------------------------------------------------------------- geometry

def nearest_on(path, lat, lon):
    """(index of segment, distance m, unit heading (east, north)) nearest to point."""
    best = (None, 1e18, (1.0, 0.0))
    c = math.cos(math.radians(lat))
    for i in range(len(path) - 1):
        ax, ay = path[i][1] * c * M_LAT, path[i][0] * M_LAT
        bx, by = path[i + 1][1] * c * M_LAT, path[i + 1][0] * M_LAT
        px_, py_ = lon * c * M_LAT, lat * M_LAT
        vx, vy = bx - ax, by - ay
        vv = vx * vx + vy * vy or 1.0
        t = max(0.0, min(1.0, ((px_ - ax) * vx + (py_ - ay) * vy) / vv))
        d = math.hypot(px_ - (ax + vx * t), py_ - (ay + vy * t))
        if d < best[1]:
            l = math.sqrt(vv)
            best = (i, d, (vx / l, vy / l))
    return best


def arrow_for(he, hn):
    if abs(he) >= abs(hn):
        return "→" if he > 0 else "←"
    return "↑" if hn > 0 else "↓"


def clip(path, lat, lon, radius):
    """The run of `path` around the point within `radius` metres, kept whole
    (no gaps), so a vehicle can travel along it."""
    if not path:
        return []
    i0, _, _ = nearest_on(path, lat, lon)
    a = i0
    while a > 0 and metres(lat, lon, *path[a - 1]) < radius:
        a -= 1
    b = i0 + 1
    while b < len(path) - 1 and metres(lat, lon, *path[b + 1]) < radius:
        b += 1
    return path[a:b + 1]


def simplify(path, tol=4.0):
    """Drop points closer than tol metres to the last kept one."""
    out = []
    for p in path:
        if not out or metres(out[-1][0], out[-1][1], p[0], p[1]) >= tol:
            out.append(p)
    if path and out[-1] != path[-1]:
        out.append(path[-1])
    return out


# ---------------------------------------------------------------- choosing

def short_name(gtfs_name):
    """"Amsterdam, Vijzelgracht" -> "Vijzelgracht"."""
    return gtfs_name.split(", ", 1)[1] if ", " in gtfs_name else gtfs_name


def shorten_dest(name):
    n = re.sub(r"^Station\s+", "", name)
    n = re.sub(r"(?<=\w)station$", "", n)
    return n.strip() or name


def find(args, platforms):
    if args.address:
        q = urllib.parse.urlencode({"q": args.address, "format": "json", "limit": 1,
                                    "countrycodes": "nl"})
        res = json.loads(http("https://nominatim.openstreetmap.org/search?" + q))
        if not res:
            sys.exit("address not found: %s" % args.address)
        lat, lon = float(res[0]["lat"]), float(res[0]["lon"])
        log("address: %s -> %.5f, %.5f" % (res[0].get("display_name", "")[:70], lat, lon))
    else:
        lat, lon = args.lat, args.lon

    near = sorted(((metres(lat, lon, p[1], p[2]), tpc, p) for tpc, p in platforms.items()
                   if metres(lat, lon, p[1], p[2]) < args.radius), key=lambda x: x[0])
    if not near:
        sys.exit("no platforms within %d m; try --radius" % args.radius)
    live = ovapi_tpcs([t for _, t, _ in near])
    near = [x for x in near if x[1] in live]
    if not near:
        sys.exit("platforms found, but none with departures right now")

    name = args.stop or near[0][2][0]
    picked = [(tpc, p) for _, tpc, p in near
              if p[0] == name or short_name(p[0]) == name]
    want = set(args.lines.split(",")) if args.lines else None

    log("\nplatforms of %s:" % short_name(picked[0][1][0]))
    for tpc, p in picked:
        info = live[tpc]
        log("  %s  %-5s %s" % (tpc, info["type"], ", ".join(
            "%s>%s" % (l, "/".join(sorted(v["dests"]))) for l, v in sorted(info["lines"].items()))))
    others = sorted({short_name(p[0]) for _, t, p in near if p[0] != name})
    if others:
        log("other stops within %d m: %s (use --stop)" % (args.radius, ", ".join(others)))

    directions = []
    for tpc, p in picked:
        info = live[tpc]
        lines = sorted(l for l in info["lines"] if not want or l in want)
        if not lines:
            continue
        dests = {}
        for l in lines:
            for dname, n in info["lines"][l]["dests"].items():
                if short_name(dname) != short_name(name):   # drop "terminates here"
                    dests[dname] = dests.get(dname, 0) + n
        if not dests:
            continue
        label = ", ".join(shorten_dest(d) for d in sorted(dests, key=dests.get, reverse=True)[:2])
        directions.append({"tpc": tpc, "label": label, "lines": lines,
                           "type": info["type"], "dests": sorted(dests)})
    if not directions:
        sys.exit("no platform carries the chosen lines")

    lines = {}
    for tpc, p in picked:
        for l, v in live[tpc]["lines"].items():
            if any(l in d["lines"] for d in directions):
                lines[l] = {"plan": v["plan"], "type": v["type"]}
    operator = live[picked[0][0]]["operator"]
    cfg = {
        "name": short_name(name),
        "operator": operator,
        "lines": lines,
        "directions": directions,
        "aliases": {},
    }
    return cfg


# ---------------------------------------------------------------- build

def build(cfg, platforms):
    slug = re.sub(r"[^a-z0-9]+", "-", cfg["name"].lower()).strip("-")
    for d in cfg["directions"]:
        p = platforms.get(d["tpc"])
        if not p:
            sys.exit("timing point %s not in GTFS stops" % d["tpc"])
        d["lat"], d["lon"] = p[1], p[2]
    lat0 = sum(d["lat"] for d in cfg["directions"]) / len(cfg["directions"])
    lon0 = sum(d["lon"] for d in cfg["directions"]) / len(cfg["directions"])

    # Which lines have GPS in the national feed right now.
    gps = gps_lines(cfg["operator"])
    for l, v in cfg["lines"].items():
        v["gps"] = v["plan"] in gps
    for d in cfg["directions"]:
        d["gps"] = all(cfg["lines"][l]["gps"] for l in d["lines"])
    log("gps: %s" % ", ".join("%s=%s" % (l, "yes" if v["gps"] else "no")
                               for l, v in sorted(cfg["lines"].items())))

    # Colours: surface (GPS) lines first, then the rest, unless the config sets one.
    order = sorted(cfg["lines"], key=lambda l: (not cfg["lines"][l]["gps"],
                                                len(l), l))
    for i, l in enumerate(order):
        cfg["lines"][l].setdefault("colour", "#%02X%02X%02X" % PALETTE[i % len(PALETTE)])

    # Route shapes: one relation per line and direction, from OSM.
    refs_by_type = {}
    for l, v in cfg["lines"].items():
        refs_by_type.setdefault(v["type"], set()).add(l)
    routes = osm_routes(lat0, lon0, refs_by_type, slug)["elements"]
    rels = [e for e in routes if e["type"] == "relation"]
    for d in cfg["directions"]:
        # The relation for this direction: same line, its "to" sharing words
        # with this platform's destinations, and passing the platform.
        best, best_score = None, -1e9
        for e in rels:
            if e["tags"].get("ref") not in d["lines"]:
                continue
            path = chain(e)
            if len(path) < 2:
                continue
            _, dist, _ = nearest_on(path, d["lat"], d["lon"])
            dt = set().union(*(tokens(x) for x in d["dests"]))
            score = 10 * len(tokens(e["tags"].get("to")) & dt) - dist / 20
            if score > best_score:
                best, best_score = path, score
        if best:
            i, dist, (he, hn) = nearest_on(best, d["lat"], d["lon"])
            d["heading"] = [he, hn]
            d["arrow"] = d.get("arrow") or arrow_for(he, hn)
            d["path"] = simplify(clip(best, d["lat"], d["lon"], 2200))
        else:
            log("route: no OSM shape for %s at %s; no arrow from the map" % (d["lines"], d["tpc"]))
            d["arrow"] = d.get("arrow") or "→"
            d["path"] = []

    rows = make_rows(cfg)
    write_stop_h(cfg, rows, lat0, lon0)
    render_maps(cfg, lat0, lon0, osm_base(lat0, lon0, slug)["elements"], routes)

    # Keep the human-editable part of the config.
    keep = {
        "name": cfg["name"], "operator": cfg["operator"], "aliases": cfg.get("aliases", {}),
        "lines": {l: {"plan": v["plan"], "type": v["type"], "colour": v["colour"]}
                  for l, v in cfg["lines"].items()},
        "directions": [{k: d[k] for k in ("tpc", "label", "arrow", "lines", "type", "dests")}
                       for d in cfg["directions"]],
    }
    os.makedirs(os.path.join(ROOT, "stops"), exist_ok=True)
    out = os.path.join(ROOT, "stops", slug + ".json")
    json.dump(keep, open(out, "w"), indent=2, ensure_ascii=False)
    log("\nwrote %s, src/stop.h, src/map_data.h" % os.path.relpath(out, ROOT))


def make_rows(cfg):
    """Rows on the face (at most 3). A pair of directions without GPS on the
    same lines shares one row, like a metro both ways; every other direction
    gets its own. If there are still too many, pairs of the same type merge."""
    dirs = list(range(len(cfg["directions"])))
    D = cfg["directions"]
    rows, used = [], set()
    for i in dirs:
        if i in used or D[i]["gps"]:
            continue
        for j in dirs:
            if j > i and j not in used and not D[j]["gps"] and D[j]["lines"] == D[i]["lines"]:
                rows.append([i, j])
                used |= {i, j}
                break
    for i in dirs:
        if i not in used and D[i]["gps"]:
            rows.insert(len([r for r in rows if len(r) == 1]), [i])
            used.add(i)
    for i in dirs:
        if i not in used:
            rows.append([i])
            used.add(i)
    while len(rows) > 3:
        singles = [r for r in rows if len(r) == 1]
        merged = False
        for a in singles:
            for b in singles:
                if a is not b and D[a[0]]["type"] == D[b[0]]["type"]:
                    rows.remove(b)
                    a.append(b[0])
                    merged = True
                    break
            if merged:
                break
        if not merged:
            log("rows: more than 3; dropping %s" % [D[i]["tpc"] for i in rows[-1]])
            rows.pop()
    out = []
    for r in rows:
        if len(r) == 2:
            t = TYPE_NAME.get(D[r[0]]["type"], D[r[0]]["type"].title())
            out.append({"dirs": r, "label": "%s %s" % (t, ", ".join(D[r[0]]["lines"]))})
        else:
            out.append({"dirs": r, "label": ""})
    return out


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_stop_h(cfg, rows, lat0, lon0):
    D = cfg["directions"]
    L = cfg["lines"]
    path = os.path.join(ROOT, "src", "stop.h")
    rgb = lambda h: tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))
    with open(path, "w") as f:
        f.write("// Generated by scripts/setup.py from stops/*.json. Do not edit; rerun\n"
                "// setup.py --config stops/<stop>.json.\n#pragma once\n#include <stdint.h>\n\n")
        f.write("#define STOP_NAME %s\n" % c_str(cfg["name"]))
        f.write('#define STOP_LAT "%.5f"\n#define STOP_LON "%.5f"\n' % (lat0, lon0))
        f.write("#define OPERATOR %s\n" % c_str(cfg["operator"]))
        f.write("#define LINE_FILTER %s\n" % c_str(",".join(sorted(L, key=lambda x: (len(x), x)))))
        gps_plan = [L[l]["plan"] for l in sorted(L) if L[l]["gps"]]
        f.write("#define POSITION_LINES %s\n" % c_str(",".join(gps_plan)))
        f.write("#define HAS_GPS_LINES %d\n\n" % (1 if gps_plan else 0))

        f.write("#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))\n")
        f.write("struct LineInfo {\n  const char *pub;   // public number, as on the vehicle\n"
                "  const char *plan;  // planning number, as in the GPS feed\n"
                "  uint16_t rgb565;\n  bool gps;\n};\n")
        f.write("static const LineInfo LINES[] = {\n")
        for l in sorted(L, key=lambda x: (len(x), x)):
            r, g, b = rgb(L[l]["colour"])
            f.write("    {%s, %s, RGB(0x%02X, 0x%02X, 0x%02X), %s},\n"
                    % (c_str(l), c_str(L[l]["plan"]), r, g, b, "true" if L[l]["gps"] else "false"))
        f.write("};\n\n")

        f.write("struct DirectionConfig {\n  const char *label;\n  const char *arrow;\n"
                "  const char *line;   // its main line, for colours\n"
                "  const char *tpcs;   // OVapi timing point codes, comma-separated\n"
                "  bool gps;           // vehicles report positions\n"
                "  float lat, lon;     // the platform\n"
                "  float estMps;       // without GPS: estimated speed along its path\n};\n")
        f.write("static const uint8_t NUM_DIRECTIONS = %d;\n" % len(D))
        f.write("static const DirectionConfig DIRECTIONS[NUM_DIRECTIONS] = {\n")
        for d in D:
            f.write("    {%s, %s, %s, %s, %s, %.6ff, %.6ff, %.1ff},\n"
                    % (c_str(d["label"]), c_str(d["arrow"]), c_str(d["lines"][0]), c_str(d["tpc"]),
                       "true" if d["gps"] else "false", d["lat"], d["lon"],
                       EST_MPS.get(d["type"], 6.0)))
        f.write("};\n\n")

        f.write("// Rows on the face: one direction each, or a pair sharing a row.\n")
        f.write("struct RowConfig {\n  const char *label;  // pair rows and table headings\n"
                "  uint8_t dirs[2];\n  uint8_t count;\n};\n")
        f.write("static const uint8_t NUM_ROWS = %d;\n" % len(rows))
        f.write("static const RowConfig ROWS[NUM_ROWS] = {\n")
        for r in rows:
            ds = r["dirs"] + [0] * (2 - len(r["dirs"]))
            f.write("    {%s, {%d, %d}, %d},\n" % (c_str(r["label"]), ds[0], ds[1], len(r["dirs"])))
        f.write("};\n\n")

        aliases = {}
        for d in D:
            for x in d["dests"]:
                s = shorten_dest(x)
                if s != x:
                    aliases[x] = s
        aliases.update(cfg.get("aliases") or {})
        f.write("struct DestAlias {\n  const char *from;\n  const char *to;\n};\n")
        f.write("static const DestAlias DEST_ALIASES[] = {\n")
        for k, v in sorted(aliases.items()):
            f.write("    {%s, %s},\n" % (c_str(k), c_str(v)))
        f.write("    {\"\", \"\"},\n};\n")
    log("wrote src/stop.h: %d directions, %d rows, %d lines" % (len(D), len(rows), len(L)))


# ---------------------------------------------------------------- map

BG = (0, 0, 0)
WATER = (12, 24, 38)
STREET = {
    "primary": ((34, 34, 36), 14), "secondary": ((32, 32, 34), 12),
    "tertiary": ((28, 28, 30), 10), "unclassified": ((22, 22, 24), 7),
    "residential": ((22, 22, 24), 7), "living_street": ((20, 20, 22), 6),
    "pedestrian": ((18, 18, 20), 6),
}
TRACK = ((92, 92, 98), 4.5)
RAIL = ((60, 60, 66), 4.0)
LINE_PX = 1.7         # one line's stroke, screen px, at every zoom
LINE_BRIGHT = 0.78    # lines sit under the badges, so a little dimmer
UNDER_BRIGHT = 0.5    # lines without GPS (underground), dimmer still


def render_maps(cfg, lat0, lon0, base, routes):
    from PIL import Image, ImageDraw

    m_lon = M_LAT * math.cos(math.radians(lat0))
    rgb = lambda h, k: tuple(int(int(h[i:i + 2], 16) * k) for i in (1, 3, 5))

    # Which ways each line uses, so shared track is drawn side by side.
    way_lines = {}
    for e in routes:
        if e["type"] != "relation":
            continue
        ref = e["tags"].get("ref")
        if ref not in cfg["lines"]:
            continue
        for mbr in e.get("members", []):
            if mbr["type"] == "way" and mbr.get("geometry") and mbr.get("role") in ("", None, "forward", "backward"):
                g, refs = way_lines.setdefault(mbr["ref"], (mbr["geometry"], set()))
                refs.add(ref)

    def render(radius):
        mpp = radius / (SIZE / 2)

        def px(la, lo):
            return ((SIZE / 2 + (lo - lon0) * m_lon / mpp) * SS,
                    (SIZE / 2 - (la - lat0) * M_LAT / mpp) * SS)

        def pts(g):
            return [px(p["lat"], p["lon"]) for p in g if p]

        def w(m):
            return max(1, int(round(m / mpp * SS)))

        img = Image.new("RGB", (SIZE * SS, SIZE * SS), BG)
        d = ImageDraw.Draw(img)
        for e in base:
            t = e.get("tags", {})
            if e["type"] == "way" and t.get("natural") == "water":
                p = pts(e.get("geometry", []))
                if len(p) >= 3:
                    d.polygon(p, fill=WATER)
            if e["type"] == "relation" and t.get("natural") == "water":
                for role, fill in (("outer", WATER), ("inner", BG)):
                    for mb in e.get("members", []):
                        if mb.get("role") == role and mb.get("geometry"):
                            p = pts(mb["geometry"])
                            if len(p) >= 3:
                                d.polygon(p, fill=fill)
        for e in base:
            if e["type"] == "way" and e.get("tags", {}).get("waterway") in ("canal", "river"):
                p = pts(e.get("geometry", []))
                if len(p) >= 2:
                    d.line(p, fill=WATER, width=w(14), joint="curve")
        for cls in ("pedestrian", "living_street", "residential", "unclassified",
                    "tertiary", "secondary", "primary"):
            col, mw = STREET[cls]
            for e in base:
                if e["type"] == "way" and e.get("tags", {}).get("highway") == cls:
                    p = pts(e.get("geometry", []))
                    if len(p) >= 2:
                        d.line(p, fill=col, width=w(mw), joint="curve")
        for e in base:
            rw = e.get("tags", {}).get("railway") if e["type"] == "way" else None
            if rw in ("tram", "rail", "light_rail"):
                col, mw = TRACK if rw == "tram" else RAIL
                p = pts(e.get("geometry", []))
                if len(p) >= 2:
                    d.line(p, fill=col, width=w(mw), joint="curve")

        # Lines without GPS (underground): one solid, dim stroke per direction path.
        for dd in cfg["directions"]:
            if dd["gps"] or not dd["path"]:
                continue
            col = rgb(cfg["lines"][dd["lines"][0]]["colour"], LINE_BRIGHT * UNDER_BRIGHT)
            d.line([px(a, b) for a, b in dd["path"]], fill=col,
                   width=int(LINE_PX * SS), joint="curve")
        # Surface lines in colour, side by side where they share a way.
        for g, refs in way_lines.values():
            refs = sorted(r for r in refs if cfg["lines"][r]["gps"])
            if not refs:
                continue
            p = pts(g)
            if len(p) < 2:
                continue
            for i, ref in enumerate(sorted(refs, key=lambda x: (len(x), x))):
                off = (i - (len(refs) - 1) / 2) * LINE_PX * SS
                d.line(offset_line(p, off), fill=rgb(cfg["lines"][ref]["colour"], LINE_BRIGHT),
                       width=int(LINE_PX * SS), joint="curve")

        img = img.resize((SIZE, SIZE), Image.LANCZOS)
        fade = Image.new("L", (SIZE, SIZE), 0)
        fp = fade.load()
        r0, r1 = SIZE / 2 - 40, SIZE / 2
        for y in range(SIZE):
            for x in range(SIZE):
                r = math.hypot(x + 0.5 - SIZE / 2, y + 0.5 - SIZE / 2)
                fp[x, y] = 255 if r <= r0 else 0 if r >= r1 else int(255 * (r1 - r) / (r1 - r0))
        return Image.composite(img, Image.new("RGB", (SIZE, SIZE), BG), fade)

    out = os.path.join(ROOT, "src", "map_data.h")
    prev_dir = os.path.join(ROOT, "work")
    os.makedirs(prev_dir, exist_ok=True)
    with open(out, "w") as f:
        f.write("// Generated by scripts/setup.py. Do not edit.\n")
        f.write("// Map data (c) OpenStreetMap contributors, ODbL.\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("static const float MAP_LAT0 = %.6ff;\nstatic const float MAP_LON0 = %.6ff;\n"
                % (lat0, lon0))
        f.write("static const uint16_t MAP_W = %d, MAP_H = %d;\n\n" % (SIZE, SIZE))
        for name, radius in LEVELS:
            img = render(radius)
            img.resize((SIZE * 2, SIZE * 2), Image.NEAREST).save(
                os.path.join(prev_dir, "map_preview_%s.png" % name.lower()))
            px565 = [((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                     for r, g, b in img.get_flattened_data()]
            f.write("static const uint16_t MAP_%s[%d] = {\n" % (name, SIZE * SIZE))
            for i in range(0, len(px565), 16):
                f.write("  " + ",".join("0x%04X" % v for v in px565[i:i + 16]) + ",\n")
            f.write("};\n\n")
        f.write("struct MapLevel {\n  const uint16_t *pixels;\n  float mPerPx;\n  uint16_t radiusM;\n};\n")
        f.write("static const MapLevel MAP_LEVELS[] = {\n")
        for name, radius in LEVELS:
            f.write("    {MAP_%s, %.5ff, %d},\n" % (name, radius / (SIZE / 2), radius))
        f.write("};\nstatic const uint8_t MAP_LEVEL_COUNT = %d;\n\n" % len(LEVELS))

        # Per direction: heading at the platform, and its path in travel
        # order (used to place vehicles without GPS).
        f.write("struct DirPath {\n  const float (*pts)[2];   // lat, lon, in travel order\n"
                "  uint16_t n;\n  float headE, headN;      // unit heading at the platform\n};\n")
        for i, dd in enumerate(cfg["directions"]):
            p = dd["path"] if not dd["gps"] else []
            if p:
                f.write("static const float DIR_PATH_%d[][2] = {%s};\n"
                        % (i, ",".join("{%.6ff,%.6ff}" % q for q in p)))
        f.write("static const DirPath DIR_PATHS[] = {\n")
        for i, dd in enumerate(cfg["directions"]):
            he, hn = dd.get("heading", [1.0, 0.0])
            if dd["path"] and not dd["gps"]:
                f.write("    {DIR_PATH_%d, %d, %.4ff, %.4ff},\n" % (i, len(dd["path"]), he, hn))
            else:
                f.write("    {nullptr, 0, %.4ff, %.4ff},\n" % (he, hn))
        f.write("};\n")
    log("wrote src/map_data.h (%d KB of map); previews in work/" % (
        len(LEVELS) * SIZE * SIZE * 2 // 1024))


def offset_line(p, off):
    if off == 0 or len(p) < 2:
        return p
    out = []
    for i in range(len(p)):
        a, b = p[max(0, i - 1)], p[min(len(p) - 1, i + 1)]
        dx, dy = b[0] - a[0], b[1] - a[1]
        l = math.hypot(dx, dy) or 1
        out.append((p[i][0] - dy / l * off, p[i][1] + dx / l * off))
    return out


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--address", help='e.g. "Vijzelgracht, Amsterdam"')
    src.add_argument("--lat", type=float)
    src.add_argument("--config", help="stops/<stop>.json from an earlier run")
    ap.add_argument("--lon", type=float)
    ap.add_argument("--stop", help="stop name, when several are near the address")
    ap.add_argument("--lines", help="keep only these lines, e.g. 1,7,19,52")
    ap.add_argument("--label", action="append", default=[],
                    help="TPC=Label, the name shown for a direction (repeatable)")
    ap.add_argument("--radius", type=int, default=300, help="search radius, metres")
    args = ap.parse_args()
    if args.lat is not None and args.lon is None:
        ap.error("--lat needs --lon")

    platforms = gtfs_platforms()
    if args.config:
        cfg = json.load(open(args.config))
    else:
        cfg = find(args, platforms)
    for kv in args.label:
        tpc, _, label = kv.partition("=")
        for d in cfg["directions"]:
            if d["tpc"] == tpc:
                d["label"] = label
    build(cfg, platforms)


if __name__ == "__main__":
    main()
