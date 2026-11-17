// tinytram positions: live vehicle positions near one point, for one Dutch
// operator and a few lines, small enough for an ESP32.
//
// Upstream is OVapi's national GTFS-realtime vehicle positions feed (~570 KB
// protobuf, every operator in the Netherlands). This Worker fetches it at most
// every 15 s per colo, keeps GVB vehicles on the requested lines within the
// requested radius, and returns compact JSON. No dependencies: the protobuf
// subset it needs is decoded by hand below.
//
//   GET /v?lat=52.3603&lon=4.8911&r=1600&op=GVB&lines=1,7,19
//   op defaults to GVB; it is the operator part of the feed's entity id
//   -> {"t":1794848544,"v":[{"l":"7","j":15623,"d":1,"la":52.35911,"lo":4.89134,"a":42}]}
//
//   l  line (GVB LinePlanningNumber, same as the public number for trams)
//   j  journey number, matches OVapi's JourneyNumber for the same trip
//   d  GTFS direction_id
//   la/lo  position, 5 decimals (~1 m)
//   b  bearing in degrees, when the feed has one
//   a  age of the position in seconds at response time

const UPSTREAM = "https://gtfs.ovapi.nl/nl/vehiclePositions.pb";
const CACHE_S = 15;

interface Vehicle {
  l: string;
  j: number;
  d: number;
  la: number;
  lo: number;
  b?: number;
  a: number;
}

// ---------------- Minimal protobuf reader ----------------

class Reader {
  pos: number;
  constructor(public buf: Uint8Array, start = 0, public end = buf.length) {
    this.pos = start;
  }
  done(): boolean {
    return this.pos >= this.end;
  }
  varint(): number {
    // Values here fit in 53 bits (timestamps, small enums, lengths).
    let result = 0;
    let mul = 1;
    for (;;) {
      if (this.pos >= this.end) throw new Error("truncated varint");
      const b = this.buf[this.pos++];
      result += (b & 0x7f) * mul;
      if (b < 0x80) return result;
      mul *= 128;
      if (mul > 2 ** 63) throw new Error("varint too long");
    }
  }
  float(): number {
    if (this.pos + 4 > this.end) throw new Error("truncated float");
    const v = new DataView(this.buf.buffer, this.buf.byteOffset + this.pos, 4).getFloat32(0, true);
    this.pos += 4;
    return v;
  }
  // Returns [start, end] of a length-delimited field and skips it.
  sub(): [number, number] {
    const len = this.varint();
    const s = this.pos;
    if (s + len > this.end) throw new Error("truncated field");
    this.pos = s + len;
    return [s, s + len];
  }
  skip(wire: number): void {
    switch (wire) {
      case 0: this.varint(); break;
      case 1: this.pos += 8; break;
      case 2: this.sub(); break;
      case 5: this.pos += 4; break;
      default: throw new Error("unsupported wire type " + wire);
    }
  }
}

const utf8 = new TextDecoder();
const str = (buf: Uint8Array, [s, e]: [number, number]) => utf8.decode(buf.subarray(s, e));

interface Raw {
  directionId: number;
  lat: number;
  lon: number;
  bearing?: number;
  timestamp: number;
}

function decodeVehicle(buf: Uint8Array, s: number, e: number): Raw {
  const out: Raw = { directionId: -1, lat: 0, lon: 0, timestamp: 0 };
  const r = new Reader(buf, s, e);
  while (!r.done()) {
    const key = r.varint();
    const field = key >>> 3, wire = key & 7;
    if (field === 1 && wire === 2) {            // trip
      const [ts, te] = r.sub();
      const t = new Reader(buf, ts, te);
      while (!t.done()) {
        const k = t.varint();
        if (k >>> 3 === 6 && (k & 7) === 0) out.directionId = t.varint();
        else t.skip(k & 7);
      }
    } else if (field === 2 && wire === 2) {     // position
      const [ps, pe] = r.sub();
      const p = new Reader(buf, ps, pe);
      while (!p.done()) {
        const k = p.varint();
        const f = k >>> 3;
        if (f === 1 && (k & 7) === 5) out.lat = p.float();
        else if (f === 2 && (k & 7) === 5) out.lon = p.float();
        else if (f === 3 && (k & 7) === 5) out.bearing = p.float();
        else p.skip(k & 7);
      }
    } else if (field === 5 && wire === 0) {     // timestamp
      out.timestamp = r.varint();
    } else {
      r.skip(wire);
    }
  }
  return out;
}

// Entity ids look like "2026-11-16:GVB:7:15623" (date:operator:line:journey).
function filterFeed(buf: Uint8Array, lat: number, lon: number, radiusM: number,
                    op: string, lines: Set<string>, now: number): Vehicle[] {
  const out: Vehicle[] = [];
  const mPerDegLat = 111_320;
  const mPerDegLon = 111_320 * Math.cos((lat * Math.PI) / 180);
  const r = new Reader(buf);
  while (!r.done()) {
    const key = r.varint();
    if (key >>> 3 !== 2 || (key & 7) !== 2) { r.skip(key & 7); continue; }
    const [es, ee] = r.sub();
    const e = new Reader(buf, es, ee);
    let id = "";
    let veh: [number, number] | null = null;
    while (!e.done()) {
      const k = e.varint();
      if (k >>> 3 === 1 && (k & 7) === 2) id = str(buf, e.sub());
      else if (k >>> 3 === 4 && (k & 7) === 2) veh = e.sub();
      else e.skip(k & 7);
    }
    if (!veh) continue;
    const parts = id.split(":");
    if (parts.length < 4 || parts[1] !== op || !lines.has(parts[2])) continue;
    const v = decodeVehicle(buf, veh[0], veh[1]);
    if (!v.lat || !v.lon) continue;
    const dx = (v.lon - lon) * mPerDegLon, dy = (v.lat - lat) * mPerDegLat;
    if (dx * dx + dy * dy > radiusM * radiusM) continue;
    const item: Vehicle = {
      l: parts[2],
      j: Number(parts[3]) || 0,
      d: v.directionId,
      la: Math.round(v.lat * 1e5) / 1e5,
      lo: Math.round(v.lon * 1e5) / 1e5,
      a: v.timestamp ? Math.max(0, Math.round(now - v.timestamp)) : -1,
    };
    if (v.bearing !== undefined && Number.isFinite(v.bearing)) item.b = Math.round(v.bearing);
    out.push(item);
  }
  return out;
}

// ---------------- Request handling ----------------

function num(v: string | null, min: number, max: number): number | null {
  if (v === null || !/^-?\d+(\.\d+)?$/.test(v)) return null;
  const n = Number(v);
  return n >= min && n <= max ? n : null;
}

function bad(msg: string): Response {
  return new Response(JSON.stringify({ error: msg }), {
    status: 400,
    headers: { "content-type": "application/json" },
  });
}

export default {
  async fetch(req: Request, _env: unknown, ctx: ExecutionContext): Promise<Response> {
    const url = new URL(req.url);
    if (req.method !== "GET") return new Response("method not allowed", { status: 405 });
    if (url.pathname === "/") {
      return new Response("tinytram positions. GET /v?lat=&lon=&r=&lines=\n");
    }
    if (url.pathname !== "/v") return new Response("not found", { status: 404 });

    // Validate at the boundary; reject anything unexpected.
    const lat = num(url.searchParams.get("lat"), 50.5, 53.8);   // the Netherlands
    const lon = num(url.searchParams.get("lon"), 3.2, 7.3);
    const radius = num(url.searchParams.get("r"), 100, 5000);
    const linesRaw = url.searchParams.get("lines") ?? "";
    if (lat === null || lon === null || radius === null) return bad("lat, lon, r");
    if (!/^[0-9A-Za-z]{1,4}(,[0-9A-Za-z]{1,4}){0,15}$/.test(linesRaw)) return bad("lines");
    const lines = new Set(linesRaw.split(","));
    const op = url.searchParams.get("op") ?? "GVB";
    if (!/^[A-Z0-9]{2,12}$/.test(op)) return bad("op");

    // Normalised cache key: same query, same answer for CACHE_S seconds.
    const key = new Request(
      `${url.origin}/v?lat=${lat}&lon=${lon}&r=${radius}&op=${op}&lines=${[...lines].sort().join(",")}`,
    );
    const cache = caches.default;
    const hit = await cache.match(key);
    if (hit) return hit;

    const up = await fetch(UPSTREAM, { cf: { cacheTtl: CACHE_S, cacheEverything: true } });
    if (!up.ok) {
      return new Response(JSON.stringify({ error: "upstream " + up.status }), {
        status: 502,
        headers: { "content-type": "application/json" },
      });
    }
    const buf = new Uint8Array(await up.arrayBuffer());
    const now = Date.now() / 1000;
    let vehicles: Vehicle[];
    try {
      vehicles = filterFeed(buf, lat, lon, radius, op, lines, now);
    } catch (err) {
      return new Response(JSON.stringify({ error: "decode " + String(err) }), {
        status: 502,
        headers: { "content-type": "application/json" },
      });
    }

    const res = new Response(JSON.stringify({ t: Math.round(now), v: vehicles }), {
      headers: {
        "content-type": "application/json",
        "cache-control": `public, max-age=${CACHE_S}`,
      },
    });
    ctx.waitUntil(cache.put(key, res.clone()));
    return res;
  },
} satisfies ExportedHandler;
