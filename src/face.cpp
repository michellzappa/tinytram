// The round face: a dark map of the streets around the stop, the trams on it
// at their GPS positions, and the next departures per direction at the
// bottom. North is up; the stop is the centre of the glass.
//
// Everything is drawn into the PSRAM canvas in logical (upright)
// coordinates; board_display.cpp rotates it onto the panel.
#include "face.h"

#include <math.h>
#include <string.h>

#include "board_display.h"
#include "config.h"
#include "map_data.h"
#include "text.h"

static const uint16_t C_BG = 0x0000;
static const uint16_t C_TEXT = RGB(0xF2, 0xF2, 0xF2);
static const uint16_t C_SOFT = RGB(0x9C, 0x9C, 0x9C);
static const uint16_t C_DIM = RGB(0x58, 0x58, 0x58);
static const uint16_t C_LIVE = RGB(0x4C, 0xD9, 0x7B);
static const uint16_t C_AGE = RGB(0xF0, 0xB0, 0x3C);
static const uint16_t C_BAD = RGB(0xEC, 0x5A, 0x4C);

static const int16_t CX = SCR_W / 2;
static const int16_t CY = SCR_H / 2;

// Pixel shift against burn-in: the whole face, map included, walks a small
// orbit (main.cpp sets it).
static int16_t ox = 0, oy = 0;
static uint8_t level = 0;   // index into MAP_LEVELS
static bool tableMode = false;

static uint16_t lineColour(const char *line) {
  for (const auto &c : LINES)
    if (strcmp(c.pub, line) == 0) return c.rgb565;
  return RGB(0x9A, 0x9A, 0x9A);
}

static uint16_t scale565(uint16_t c, uint8_t a) { return blend565(C_BG, c, a); }

static int16_t chordHalf(int16_t y, int16_t r) {
  const int32_t dy = y - CY;
  if (dy * dy >= (int32_t)r * r) return 0;
  return (int16_t)sqrtf((float)((int32_t)r * r - dy * dy));
}

// ---------------- Primitives ----------------

static inline void plot(int16_t x, int16_t y, uint16_t col, uint8_t a) {
  if (x < 0 || y < 0 || x >= SCR_W || y >= SCR_H || a == 0) return;
  uint16_t *p = gfx->fb() + (int32_t)y * SCR_W + x;
  *p = blend565(*p, col, a);
}

// Anti-aliased filled disc, or a ring of `stroke` px when stroke > 0.
static void disc(float cx, float cy, float r, uint16_t col, float stroke = 0,
                 uint8_t alpha = 255) {
  const int16_t x0 = (int16_t)floorf(cx - r - 1), x1 = (int16_t)ceilf(cx + r + 1);
  const int16_t y0 = (int16_t)floorf(cy - r - 1), y1 = (int16_t)ceilf(cy + r + 1);
  for (int16_t y = y0; y <= y1; y++) {
    for (int16_t x = x0; x <= x1; x++) {
      const float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
      float a = r + 0.5f - d;
      if (stroke > 0) {
        const float inner = d - (r - stroke) + 0.5f;
        if (inner < a) a = inner;
      }
      if (a <= 0) continue;
      const uint8_t cov = a >= 1 ? 255 : (uint8_t)(a * 255);
      plot(x, y, col, (uint8_t)((cov * alpha) / 255));
    }
  }
}

// Darkens rows [y0, y1) toward black so text over the map stays readable.
// `feather` rows at each end ramp in.
static void scrim(int16_t y0, int16_t y1, uint8_t keep, int16_t feather) {
  uint16_t *fb = gfx->fb();
  for (int16_t y = y0; y < y1; y++) {
    if (y < 0 || y >= SCR_H) continue;
    int16_t edge = (int16_t)min(y - y0, y1 - 1 - y);
    uint8_t k = keep;
    if (edge < feather) k = (uint8_t)(255 - (255 - keep) * (edge + 1) / (feather + 1));
    uint16_t *row = fb + (int32_t)y * SCR_W;
    for (int16_t x = 0; x < SCR_W; x++) row[x] = blend565(C_BG, row[x], k);
  }
}

// The baked map, shifted by the burn-in offset. Rows come from flash.
static void blitMap() {
  uint16_t *fb = gfx->fb();
  for (int16_t y = 0; y < SCR_H; y++) {
    uint16_t *dst = fb + (int32_t)y * SCR_W;
    const int16_t sy = (int16_t)(y - oy);
    if (sy < 0 || sy >= MAP_H) {
      memset(dst, 0, SCR_W * 2);
      continue;
    }
    const uint16_t *src = MAP_LEVELS[level].pixels + (int32_t)sy * MAP_W;
    int16_t dx0 = ox > 0 ? ox : 0;
    int16_t sx0 = ox > 0 ? 0 : (int16_t)-ox;
    int16_t n = (int16_t)(SCR_W - (ox > 0 ? ox : -ox));
    if (dx0) memset(dst, 0, dx0 * 2);
    memcpy(dst + dx0, src + sx0, n * 2);
    if (dx0 + n < SCR_W) memset(dst + dx0 + n, 0, (SCR_W - dx0 - n) * 2);
  }
}

// Map projection: same equirectangular frame as scripts/build_map.py.
static void project(float lat, float lon, float *x, float *y) {
  static const float mLat = 111320.0f;
  static const float mLon = 111320.0f * cosf(MAP_LAT0 * (float)M_PI / 180.0f);
  const float mpp = MAP_LEVELS[level].mPerPx;
  *x = CX + ox + (lon - MAP_LON0) * mLon / mpp;
  *y = CY + oy - (lat - MAP_LAT0) * mLat / mpp;
}

static void badge(float cx, float cy, float r, const char *line, bool solid,
                  uint8_t alpha = 255) {
  const uint16_t col = scale565(lineColour(line), alpha);
  const int16_t base = (int16_t)(cy + 5);
  if (solid) {
    disc(cx + 0.5f, cy + 1.5f, r + 1.5f, C_BG, 0, 200);   // soft shadow on the map
    disc(cx, cy, r, col);
    txtCenter(inter_badge_s, line, (int16_t)cx, base, C_BG);
  } else {
    disc(cx, cy, r, C_BG, 0, 220);
    disc(cx, cy, r, col, 2.0f);
    txtCenter(inter_badge_s, line, (int16_t)cx, base, col);
  }
}

static void minutesLabel(float x, float y, int32_t s);

// Heading pointer: a small cone on the badge rim, pointing where the vehicle
// goes. (hx, hy) is a unit vector in screen space.
static void pointer(float x, float y, float r, float hx, float hy, uint16_t col,
                    uint8_t alpha = 255) {
  // Trail behind, in the line colour, fading out.
  for (float k = 0; k <= 20.0f; k += 0.7f) {
    const float f = 1.0f - k / 20.0f;
    disc(x - hx * (r + k), y - hy * (r + k), 0.6f + 2.6f * f, col, 0,
         (uint8_t)(alpha * 0.75f * f));
  }
  // White tip in front.
  const uint16_t tip = scale565(C_TEXT, alpha);
  for (float k = 0; k <= 6.0f; k += 0.4f) {
    const float rad = 3.4f * (1.0f - k / 6.4f);
    disc(x + hx * (r + 1 + k), y + hy * (r + 1 + k), rad, tip);
  }
}

// True when a point is past the visible glass.
static bool offGlass(float x, float y) {
  const float dx = x - (CX + ox), dy = y - (CY + oy);
  return dx * dx + dy * dy > 216.0f * 216.0f;
}

// ---------------- Gliding trams ----------------
//
// GPS fixes arrive every 15 s and are about a minute old. A dot that jumped
// would read as noise; each one glides from where it is drawn to its new fix
// over GLIDE_MS, eased, so the map moves the way trams do.
struct Track {
  uint32_t journey;
  char line[4];
  float fromX, fromY, toX, toY;
  uint32_t t0;
  int16_t age;
  float hx, hy;      // last heading, unit vector; valid when hasHeading
  bool hasHeading;
  bool used;
};
static Track tracks[MAX_VEHICLES];
static uint32_t seenSeq = 0;

static float ease(float t) { return t <= 0 ? 0 : t >= 1 ? 1 : t * t * (3 - 2 * t); }

static void trackPos(const Track &t, uint32_t ms, float *x, float *y) {
  const float k = ease((float)(ms - t.t0) / GLIDE_MS);
  *x = t.fromX + (t.toX - t.fromX) * k;
  *y = t.fromY + (t.toY - t.fromY) * k;
}

static void updateTracks(const Board &b, uint32_t ms) {
  if (b.vehiclesSeq == seenSeq) return;
  seenSeq = b.vehiclesSeq;
  bool keep[MAX_VEHICLES] = {};
  for (uint8_t i = 0; i < b.vehicleCount; i++) {
    const Vehicle &v = b.vehicles[i];
    float x, y;
    project(v.lat, v.lon, &x, &y);
    // Positions are stored without the burn-in offset; it is added at draw.
    x -= ox;
    y -= oy;
    int8_t slot = -1;
    for (uint8_t j = 0; j < MAX_VEHICLES; j++)
      if (tracks[j].used && tracks[j].journey == v.journey &&
          strcmp(tracks[j].line, v.line) == 0) { slot = (int8_t)j; break; }
    if (slot >= 0) {
      Track &t = tracks[slot];
      // Heading from the move between fixes, in metres-scaled pixels. A tram
      // standing still keeps the heading it had.
      const float mvx = x - t.toX, mvy = y - t.toY;
      const float mv = sqrtf(mvx * mvx + mvy * mvy);
      if (mv > 1.2f) {
        t.hx = mvx / mv;
        t.hy = mvy / mv;
        t.hasHeading = true;
      }
      float cx, cy;
      trackPos(t, ms, &cx, &cy);
      t.fromX = cx; t.fromY = cy;
      t.toX = x; t.toY = y;
      t.t0 = ms;
      t.age = v.age;
      keep[slot] = true;
      continue;
    }
    for (uint8_t j = 0; j < MAX_VEHICLES; j++) {
      if (tracks[j].used && !keep[j]) continue;
      if (tracks[j].used) continue;
      Track &t = tracks[j];
      t.used = true;
      t.journey = v.journey;
      strlcpy(t.line, v.line, sizeof(t.line));
      t.fromX = t.toX = x;
      t.fromY = t.toY = y;
      t.t0 = ms;
      t.age = v.age;
      t.hasHeading = false;
      keep[j] = true;
      break;
    }
  }
  for (uint8_t j = 0; j < MAX_VEHICLES; j++)
    if (!keep[j]) tracks[j].used = false;
}

static const Departure *departureFor(const Board &b, const Track &t, time_t now) {
  for (uint8_t i = 0; i < b.count; i++) {
    const Departure &d = b.deps[i];
    if (d.journey == t.journey && strcmp(d.line, t.line) == 0 && d.when >= now - 30)
      return &d;
  }
  return nullptr;
}

static void drawTrams(const Board &b, uint32_t ms, time_t now, bool stale) {
  const uint32_t posAge = b.vehiclesMs ? (ms - b.vehiclesMs) / 1000 : 9999;
  const bool posStale = posAge > 120;
  for (uint8_t j = 0; j < MAX_VEHICLES; j++) {
    const Track &t = tracks[j];
    if (!t.used) continue;
    float x, y;
    trackPos(t, ms, &x, &y);
    x += ox;
    y += oy;
    // Only where it really is: past the glass, it is not drawn.
    const Departure *d = departureFor(b, t, now);
    const bool coming = d && d->when > now && !stale;
    const bool pinned = false;
    if (offGlass(x, y)) continue;

    const bool fresh = !posStale && t.age >= 0 && t.age <= GPS_OLD_S;
    const uint8_t alpha = posStale || stale ? 120 : pinned ? 150 : 255;
    const float r = pinned ? 8.0f : 10.0f;
    if (t.hasHeading) pointer(x, y, r, t.hx, t.hy, scale565(lineColour(t.line), alpha), alpha);
    badge(x, y, r, t.line, fresh, alpha);
    if (coming) minutesLabel(x, y, (int32_t)(d->when - now));
  }
}

// ---------------- The station ----------------
//
// The stop's platforms are drawn live, not baked, from the coordinates in
// stop.h. A platform lights up in the line colour while its next vehicle is
// due within a minute, and breathes while it is there.

// The next departure in a direction within [-20 s, 60 s] of now, or null.
static const Departure *dueAt(const Board &b, uint8_t dir, time_t now) {
  for (uint8_t i = 0; i < b.count; i++) {
    const Departure &d = b.deps[i];
    if (d.dir == dir && !d.cancelled && d.when >= now - 20 && d.when <= now + 60)
      return &d;
  }
  return nullptr;
}

// Centroid of a platform, in screen pixels.
static void centroid(const float (*ll)[2], uint8_t n, float *x, float *y) {
  float sx = 0, sy = 0;
  for (uint8_t i = 0; i < n; i++) {
    float px, py;
    project(ll[i][0], ll[i][1], &px, &py);
    sx += px;
    sy += py;
  }
  *x = sx / n;
  *y = sy / n;
}

// Stadium (capsule) around segment a-b with radius R: a dark fill so the
// tracks under it drop away, and an anti-aliased outline of `stroke` px.
static void capsule(float ax, float ay, float bx, float by, float R, float stroke,
                    uint16_t col, uint8_t fillAlpha) {
  const float vx = bx - ax, vy = by - ay;
  const float vv = vx * vx + vy * vy;
  const int16_t x0 = (int16_t)floorf(min(ax, bx) - R - 2), x1 = (int16_t)ceilf(max(ax, bx) + R + 2);
  const int16_t y0 = (int16_t)floorf(min(ay, by) - R - 2), y1 = (int16_t)ceilf(max(ay, by) + R + 2);
  for (int16_t y = y0; y <= y1; y++) {
    for (int16_t x = x0; x <= x1; x++) {
      float t = vv > 0 ? ((x - ax) * vx + (y - ay) * vy) / vv : 0;
      t = t < 0 ? 0 : t > 1 ? 1 : t;
      const float dx = x - (ax + vx * t), dy = y - (ay + vy * t);
      const float d = sqrtf(dx * dx + dy * dy);
      const float inside = R + 0.5f - d;   // > 0 inside
      if (inside <= 0) continue;
      if (fillAlpha) plot(x, y, C_BG, inside >= 1 ? fillAlpha : (uint8_t)(inside * fillAlpha));
      float a = inside;
      const float inner = d - (R - stroke) + 0.5f;
      if (inner < a) a = inner;
      if (a > 0) plot(x, y, col, a >= 1 ? 255 : (uint8_t)(a * 255));
    }
  }
}

// The stop, drawn the way a transit map draws one. Platforms of lines with
// GPS (trams, buses) sit in a white capsule with a tick per direction, lit in
// the line colour while its vehicle is due. Each pair of directions without
// GPS (a metro both ways) is a small hollow ring in its line colour, filled
// while a train is due, joined to the capsule by a connector.
static void drawStation(const Board &b, time_t now, uint32_t ms) {
  const bool clockOk = now > 1700000000;
  float px[NUM_DIRECTIONS], py[NUM_DIRECTIONS];
  float mx = 0, my = 0;
  uint8_t ns = 0;
  for (uint8_t i = 0; i < NUM_DIRECTIONS; i++) {
    project(DIRECTIONS[i].lat, DIRECTIONS[i].lon, &px[i], &py[i]);
    if (DIRECTIONS[i].gps) { mx += px[i]; my += py[i]; ns++; }
  }

  float R = 7.5f;
  const float half = 9.0f;   // capsule straight part, each side of the centre
  if (ns) {
    mx /= ns;
    my /= ns;
    // Axis: travel heading at the first surface platform (screen y is down).
    float ux = 1, uy = 0;
    for (uint8_t i = 0; i < NUM_DIRECTIONS; i++)
      if (DIRECTIONS[i].gps) { ux = DIR_PATHS[i].headE; uy = -DIR_PATHS[i].headN; break; }
    const float ul = sqrtf(ux * ux + uy * uy);
    if (ul > 0.01f) { ux /= ul; uy /= ul; } else { ux = 1; uy = 0; }
    const float nx = -uy, ny = ux;   // normal

    float spread = 0;
    for (uint8_t i = 0; i < NUM_DIRECTIONS; i++)
      if (DIRECTIONS[i].gps)
        spread = max(spread, fabsf((px[i] - mx) * nx + (py[i] - my) * ny));
    R = max(7.5f, spread + 5.0f);
    capsule(mx - ux * half, my - uy * half, mx + ux * half, my + uy * half, R, 1.6f,
            C_TEXT, 210);

    // Ticks: one per surface direction, on its own side of the centre line.
    uint8_t k = 0;
    for (uint8_t i = 0; i < NUM_DIRECTIONS; i++) {
      if (!DIRECTIONS[i].gps) continue;
      float side = (px[i] - mx) * nx + (py[i] - my) * ny;
      if (fabsf(side) < 2.5f) side = (k % 2 ? 2.5f : -2.5f);
      const float off = side > 0 ? min(side, R - 4.0f) : max(side, -(R - 4.0f));
      const float tx = mx + nx * off, ty = my + ny * off;
      const Departure *due = clockOk ? dueAt(b, i, now) : nullptr;
      const uint16_t col = due ? lineColour(due->line) : C_SOFT;
      if (due) {
        const float ph = 0.5f + 0.5f * sinf(ms / 450.0f);
        for (float q = -half; q <= half; q += 1.0f)
          disc(tx + ux * q, ty + uy * q, 4.0f, col, 0, (uint8_t)(40 + 60 * ph));
      }
      for (float q = -half + 1; q <= half - 1; q += 0.75f)
        disc(tx + ux * q, ty + uy * q, 1.4f, col);
      k++;
    }
  }

  // Rings for directions without GPS, one per row (a pair shares a ring).
  for (uint8_t r = 0; r < NUM_ROWS; r++) {
    const RowConfig &row = ROWS[r];
    if (DIRECTIONS[row.dirs[0]].gps) continue;
    float sx = 0, sy = 0;
    const Departure *m = nullptr;
    for (uint8_t j = 0; j < row.count; j++) {
      const uint8_t i = row.dirs[j];
      sx += px[i];
      sy += py[i];
      if (!m && clockOk) m = dueAt(b, i, now);
    }
    sx /= row.count;
    sy /= row.count;
    const float rr = 5.0f;
    if (ns) {
      const float dx = sx - mx, dy = sy - my;
      const float dl = sqrtf(dx * dx + dy * dy);
      if (dl > R + rr + 2) {
        const float kx = dx / dl, ky = dy / dl;
        for (float q = R + 0.5f; q <= dl - rr; q += 0.75f)
          disc(mx + kx * q, my + ky * q, 0.8f, C_TEXT);
      }
    }
    const uint16_t col = lineColour(DIRECTIONS[row.dirs[0]].line);
    disc(sx, sy, rr + 1.5f, C_BG, 0, 220);
    if (m) {
      const float ph = 0.5f + 0.5f * sinf(ms / 450.0f);
      disc(sx, sy, rr + 3.0f, col, 0, (uint8_t)(50 + 60 * ph));
      disc(sx, sy, rr, col);
    } else {
      disc(sx, sy, rr, col, 1.6f);
    }
  }
}

// ---------------- Vehicles without GPS, estimated ----------------
//
// A metro underground reports no position. Each such vehicle is placed on its
// direction's path from its live arrival time: it is (seconds to arrival) x
// estMps metres before the platform along the path, and after departure that
// far past it. They draw as solid badges, the same as trams.
static float *pathCum[NUM_DIRECTIONS];
static float pathStopS[NUM_DIRECTIONS];
static bool pathsReady = false;

static void pathsInit() {
  if (pathsReady) return;
  pathsReady = true;
  static const float mLat = 111320.0f;
  const float mLon = 111320.0f * cosf(MAP_LAT0 * (float)M_PI / 180.0f);
  for (uint8_t d = 0; d < NUM_DIRECTIONS; d++) {
    const DirPath &p = DIR_PATHS[d];
    pathCum[d] = nullptr;
    if (!p.pts || p.n < 2) continue;
    pathCum[d] = (float *)malloc(sizeof(float) * p.n);
    if (!pathCum[d]) continue;
    pathCum[d][0] = 0;
    float best = 1e12f;
    pathStopS[d] = 0;
    for (uint16_t i = 0; i < p.n; i++) {
      if (i) {
        const float dy = (p.pts[i][0] - p.pts[i - 1][0]) * mLat;
        const float dx = (p.pts[i][1] - p.pts[i - 1][1]) * mLon;
        pathCum[d][i] = pathCum[d][i - 1] + sqrtf(dx * dx + dy * dy);
      }
      const float sy = (p.pts[i][0] - DIRECTIONS[d].lat) * mLat;
      const float sx = (p.pts[i][1] - DIRECTIONS[d].lon) * mLon;
      if (sx * sx + sy * sy < best) { best = sx * sx + sy * sy; pathStopS[d] = pathCum[d][i]; }
    }
  }
}

// Point at distance s (metres from the path start) along direction d's path.
static bool pathAt(uint8_t d, float s, float *lat, float *lon) {
  const DirPath &p = DIR_PATHS[d];
  if (!pathCum[d] || s < 0 || s > pathCum[d][p.n - 1]) return false;
  uint16_t i = 1;
  while (i < p.n - 1 && pathCum[d][i] < s) i++;
  const float seg = pathCum[d][i] - pathCum[d][i - 1];
  const float t = seg > 0 ? (s - pathCum[d][i - 1]) / seg : 0;
  *lat = p.pts[i - 1][0] + (p.pts[i][0] - p.pts[i - 1][0]) * t;
  *lon = p.pts[i - 1][1] + (p.pts[i][1] - p.pts[i - 1][1]) * t;
  return true;
}

// Minutes next to a vehicle. Under a minute it needs no label: it is here.
static void minutesLabel(float x, float y, int32_t s) {
  if (s < 60) return;
  char m[8];
  snprintf(m, sizeof(m), "%ld'", (long)(s / 60));
  const int16_t tx = (int16_t)(x + 14), ty = (int16_t)(y + 5);
  for (int8_t ddx = -1; ddx <= 1; ddx++)
    for (int8_t ddy = -1; ddy <= 1; ddy++)
      if (ddx || ddy) txtDraw(inter_badge_s, m, tx + ddx, ty + ddy, C_BG);
  txtDraw(inter_badge_s, m, tx, ty, C_TEXT);
}

static void drawEstimated(const Board &b, double nowF, bool stale) {
  pathsInit();
  if (stale) return;
  for (uint8_t i = 0; i < b.count; i++) {
    const Departure &d = b.deps[i];
    if (DIRECTIONS[d.dir].gps || d.cancelled || !pathCum[d.dir]) continue;
    const float t = (float)((double)d.when - nowF);
    if (t < -30 || t > 300) continue;   // the feed drops a vehicle once it has PASSED
    const float s = pathStopS[d.dir] - t * DIRECTIONS[d.dir].estMps;
    float lat, lon, x, y;
    if (!pathAt(d.dir, s, &lat, &lon)) continue;
    project(lat, lon, &x, &y);
    if (offGlass(x, y)) continue;   // only where it is: past the glass, not drawn
    // Heading: along the path, toward where it is going.
    float la2, lo2, x2, y2;
    if (pathAt(d.dir, s + 15.0f, &la2, &lo2)) {
      project(la2, lo2, &x2, &y2);
      const float hl = sqrtf((x2 - x) * (x2 - x) + (y2 - y) * (y2 - y));
      if (hl > 0.01f) pointer(x, y, 10, (x2 - x) / hl, (y2 - y) / hl, lineColour(d.line));
    }
    badge(x, y, 10, d.line, true);
    if (t > 0) minutesLabel(x, y, (int32_t)t);
  }
}

// ---------------- Text overlays ----------------

static void upperCopy(const char *in, char *out, size_t cap) {
  size_t i = 0;
  for (; in[i] && i + 1 < cap; i++)
    out[i] = (in[i] >= 'a' && in[i] <= 'z') ? (char)(in[i] - 32) : in[i];
  out[i] = 0;
}

// One direction as one row: "← Leidseplein  (1) 3  (7) 6  (19) 9".
// Items are dropped from the end until the row fits the glass.
static void directionRow(const Board &b, uint8_t dir, int16_t base, time_t now,
                         bool stale) {
  char label[32];
  snprintf(label, sizeof(label), "%s %s", DIRECTIONS[dir].arrow, DIRECTIONS[dir].label);

  const Departure *list[3];
  char mins[3][8];
  uint8_t n = 0;
  for (uint8_t i = 0; i < b.count && n < 3; i++) {
    const Departure &d = b.deps[i];
    if (d.dir != dir || d.when < now || d.cancelled) continue;
    const int32_t s = (int32_t)(d.when - now);
    snprintf(mins[n], sizeof(mins[0]), "%ld", (long)(s / 60));
    list[n++] = &d;
  }

  const int16_t r = 10, gapIn = 5, gapItem = 14, gapLabel = 14;
  const int16_t labelW = txtWidth(inter_unit, label);
  const int16_t maxW = (int16_t)(2 * chordHalf((int16_t)(base - 6 + oy), 212));
  int16_t itemW[3];
  int16_t total;
  for (;;) {
    total = labelW;
    for (uint8_t i = 0; i < n; i++) {
      itemW[i] = (int16_t)(2 * r + gapIn + txtWidth(inter_unit, mins[i]));
      total = (int16_t)(total + (i == 0 ? gapLabel : gapItem) + itemW[i]);
    }
    if (total <= maxW || n == 0) break;
    n--;
  }

  int16_t x = (int16_t)(CX + ox - total / 2);
  const int16_t y = (int16_t)(base + oy);
  txtDraw(inter_unit, label, x, y, C_SOFT);
  x = (int16_t)(x + labelW);
  if (n == 0) {
    txtDraw(inter_unit, stale ? "  -" : "  none soon", x, y, C_DIM);
    return;
  }
  for (uint8_t i = 0; i < n; i++) {
    x = (int16_t)(x + (i == 0 ? gapLabel : gapItem));
    const Departure &d = *list[i];
    badge(x + r, y - 6, r, d.line, d.live && !stale);
    const bool isNow = strcmp(mins[i], "0") == 0;
    txtDraw(inter_unit, mins[i], (int16_t)(x + 2 * r + gapIn), y,
            stale ? C_SOFT : isNow ? C_LIVE : C_TEXT);
    x = (int16_t)(x + itemW[i]);
  }
}

// Two directions sharing one row, like a metro both ways:
// "(52) ↑ Noord 3, 9   ↓ Zuid 2, 7". Falls back to one time per direction
// when two do not fit.
static void pairRow(const Board &b, const RowConfig &row, int16_t base, time_t now,
                    bool stale) {
  char part[2][40];
  bool live = false;
  const char *line = DIRECTIONS[row.dirs[0]].line;
  int16_t total = 0, partW[2] = {0, 0};
  const int16_t r = 10, gapIn = 8, gapPart = 16;
  const int16_t maxW = (int16_t)(2 * chordHalf((int16_t)(base - 6 + oy), 212));
  for (uint8_t perDir = 2; perDir >= 1; perDir--) {
    for (uint8_t k = 0; k < 2; k++) {
      const uint8_t dir = row.dirs[k];
      snprintf(part[k], sizeof(part[k]), "%s %s ", DIRECTIONS[dir].arrow, DIRECTIONS[dir].label);
      uint8_t n = 0;
      for (uint8_t i = 0; i < b.count && n < perDir; i++) {
        const Departure &d = b.deps[i];
        if (d.dir != dir || d.when < now || d.cancelled) continue;
        if (n == 0) { line = d.line; live = live || d.live; }
        const int32_t s = (int32_t)(d.when - now);
        char m[8];
        snprintf(m, sizeof(m), "%ld", (long)(s / 60));
        if (n) strlcat(part[k], ", ", sizeof(part[k]));
        strlcat(part[k], m, sizeof(part[k]));
        n++;
      }
      if (n == 0) strlcat(part[k], "-", sizeof(part[k]));
      partW[k] = txtWidth(inter_unit, part[k]);
    }
    total = (int16_t)(2 * r + gapIn + partW[0] + gapPart + partW[1]);
    if (total <= maxW) break;
  }
  int16_t x = (int16_t)(CX + ox - total / 2);
  const int16_t y = (int16_t)(base + oy);
  badge(x + r, y - 6, r, line, live && !stale);
  x = (int16_t)(x + 2 * r + gapIn);
  txtDraw(inter_unit, part[0], x, y, stale ? C_SOFT : C_TEXT);
  x = (int16_t)(x + partW[0] + gapPart);
  txtDraw(inter_unit, part[1], x, y, stale ? C_SOFT : C_TEXT);
}

static void wifiBars(int16_t x, int16_t base, int8_t rssi, uint16_t col) {
  const uint8_t bars = rssi == 0 ? 0 : rssi > -60 ? 3 : rssi > -72 ? 2 : 1;
  for (uint8_t i = 0; i < 3; i++) {
    const int16_t h = (int16_t)(4 + 3 * i);
    const uint16_t c = i < bars ? col : RGB(0x30, 0x30, 0x30);
    for (int16_t y = (int16_t)(base - h); y < base; y++)
      for (int16_t k = 0; k < 3; k++) plot((int16_t)(x + i * 5 + k), y, c, 255);
  }
}

static void ageText(uint32_t s, char *out, size_t cap) {
  if (s < 60) snprintf(out, cap, "%lus", (unsigned long)s);
  else if (s < 3600) snprintf(out, cap, "%lum", (unsigned long)(s / 60));
  else snprintf(out, cap, "%luh", (unsigned long)(s / 3600));
}

// A dot that says how much to trust the screen, the age of the data, and
// Wi-Fi strength. Age is the older of departures and positions. The dot
// breathes while a fetch is in flight.
static void statusLine(const Board &b, uint32_t ms, uint32_t depAge) {
  const int16_t cx = (int16_t)(CX + ox);
  const int16_t base = (int16_t)(CY + 188 + oy);
  char txt[32], a[12];
  uint16_t dot;
  uint32_t age = depAge;
  if (b.vehiclesMs) {
    const uint32_t pa = (ms - b.vehiclesMs) / 1000;
    if (pa > age) age = pa;
  }

  if (b.state == NET_CONNECTING) { strlcpy(txt, "joining wi-fi", sizeof(txt)); dot = C_DIM; }
  else if (b.state == NET_NO_TIME) { strlcpy(txt, "setting clock", sizeof(txt)); dot = C_DIM; }
  else if (!b.fetchedAt) {
    strlcpy(txt, b.state == NET_FETCH_FAILED ? "feed unreachable" : "loading", sizeof(txt));
    dot = b.state == NET_FETCH_FAILED ? C_BAD : C_DIM;
  } else {
    ageText(age, a, sizeof(a));
    if (b.state == NET_OFFLINE) { snprintf(txt, sizeof(txt), "offline · %s", a); dot = C_BAD; }
    else if (age <= FRESH_S) { snprintf(txt, sizeof(txt), "live · %s", a); dot = C_LIVE; }
    else if (age <= STALE_S) { snprintf(txt, sizeof(txt), "%s old", a); dot = C_AGE; }
    else { snprintf(txt, sizeof(txt), "stale · %s", a); dot = C_BAD; }
  }

  const int16_t tw = txtWidth(inter_unit, txt);
  const int16_t total = (int16_t)(10 + 8 + tw + 10 + 13);
  int16_t x = (int16_t)(cx - total / 2);
  uint8_t glow = 255;
  if (b.fetching) glow = (uint8_t)(110 + 145 * (0.5f + 0.5f * sinf(ms / 160.0f)));
  disc(x + 5, base - 5, 4.5f, scale565(dot, glow));
  x = (int16_t)(x + 18);
  txtDraw(inter_unit, txt, x, base, C_SOFT);
  x = (int16_t)(x + tw + 10);
  wifiBars(x, base, b.rssi, b.state == NET_OFFLINE ? C_BAD : C_SOFT);
}

// ---------------- Table mode ----------------
//
// The same departures as plain rows on black: no map, no vehicles. One
// section per row (see drawTable).
//
//   ← LEIDSEPLEIN
//   (1)  Osdorp            3 min
//   (7)  Slotermeer        6 min
//   METRO 52
//   (52) ↑ Noord           2 min
//
// A filled badge is live, an outline is timetable only. Minutes go amber
// when the vehicle runs two minutes or more late.
static const int16_t ROW_H = 26;

// Draws s left-aligned, cut with an ellipsis to maxW.
static void txtFit(const AaFont &f, const char *s, int16_t x, int16_t base,
                   int16_t maxW, uint16_t col) {
  if (txtWidth(f, s) <= maxW) {
    txtDraw(f, s, x, base, col);
    return;
  }
  const int16_t ell = txtWidth(f, "…");
  int16_t n = (int16_t)strlen(s);
  while (n > 0 && txtWidth(f, s, n) + ell > maxW) {
    n--;
    while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) n--;   // UTF-8 boundary
  }
  txtDraw(f, s, x, base, col, n);
  txtDraw(f, "…", (int16_t)(x + txtWidth(f, s, n)), base, col);
}

static void tableRow(const Departure &d, const char *dest, int16_t base, time_t now,
                     bool stale) {
  // One width for every row, so the columns line up. It fits the lowest row
  // (y 364) inside the glass.
  const int16_t half = 156;
  const int16_t left = (int16_t)(CX + ox - half + 6);
  const int16_t right = (int16_t)(CX + ox + half - 6);
  const int16_t y = (int16_t)(base + oy);
  badge(left + 11, y - 7, 11, d.line, d.live && !stale);

  const int32_t secs = (int32_t)(d.when - now);
  char m[12];
  snprintf(m, sizeof(m), "%ld", (long)(secs < 0 ? 0 : secs / 60));
  const uint16_t mc = d.cancelled ? C_DIM : stale ? C_SOFT
                      : d.delayS >= 120 ? C_AGE : C_TEXT;
  txtRight(inter_unit, "min", right, y, C_SOFT);
  const int16_t numRight = (int16_t)(right - txtWidth(inter_unit, "min") - 5);
  txtRight(inter_dest, m, numRight, y, mc);
  const int16_t destX = (int16_t)(left + 22 + 10);
  const int16_t destW = (int16_t)(numRight - txtWidth(inter_dest, m) - 12 - destX);
  txtFit(inter_dest, d.cancelled ? "cancelled" : dest, destX, y, destW,
         d.cancelled ? C_BAD : C_TEXT);
}

static void tableLabel(const char *text, int16_t base) {
  char up[40];
  upperCopy(text, up, sizeof(up));
  txtCenter(inter_label, up, (int16_t)(CX + ox), (int16_t)(base + oy), C_DIM, 2);
}

// Rows for one direction; returns how many it drew. All rows share one
// width (tableRow), so the table is a grid, not a circle.
static uint8_t tableSection(const Board &b, uint8_t dir, int16_t base, uint8_t max,
                            time_t now, bool stale, const char *destOverride) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < b.count && n < max; i++) {
    const Departure &d = b.deps[i];
    if (d.dir != dir || d.when < now) continue;
    tableRow(d, destOverride ? destOverride : d.dest, (int16_t)(base + n * ROW_H), now, stale);
    n++;
  }
  if (!n) txtCenter(inter_unit, "no departures", (int16_t)(CX + ox), (int16_t)(base + oy), C_DIM);
  return n;
}

// Sections top to bottom, one per row. A single-direction row lists its next
// departures; a pair row lists the next one each way. The space between the
// header and the status line is shared out.
static void drawTable(const Board &b, time_t now, bool stale) {
  const int16_t top = 96, bottom = 372, labelH = 26;
  uint8_t singles = 0, pairs = 0;
  for (uint8_t r = 0; r < NUM_ROWS; r++) (ROWS[r].count == 2 ? pairs : singles)++;
  const int16_t free = (int16_t)((bottom - top) / ROW_H - NUM_ROWS - 2 * pairs);
  const uint8_t perSingle = singles ? (uint8_t)constrain(free / singles, 1, 6) : 0;
  int16_t y = top;
  for (uint8_t r = 0; r < NUM_ROWS; r++) {
    const RowConfig &row = ROWS[r];
    char label[48];
    if (row.count == 2) {
      tableLabel(row.label, y);
      y = (int16_t)(y + labelH);
      for (uint8_t k = 0; k < 2; k++) {
        const uint8_t dir = row.dirs[k];
        snprintf(label, sizeof(label), "%s %s", DIRECTIONS[dir].arrow, DIRECTIONS[dir].label);
        tableSection(b, dir, y, 1, now, stale, label);
        y = (int16_t)(y + ROW_H);
      }
    } else {
      const uint8_t dir = row.dirs[0];
      snprintf(label, sizeof(label), "%s %s", DIRECTIONS[dir].arrow, DIRECTIONS[dir].label);
      tableLabel(label, y);
      y = (int16_t)(y + labelH);
      tableSection(b, dir, y, perSingle, now, stale, nullptr);
      y = (int16_t)(y + perSingle * ROW_H);
    }
    y = (int16_t)(y + 4);
  }
}

void faceToggleTable() { tableMode = !tableMode; }
bool faceTableMode() { return tableMode; }
uint8_t faceZoom() { return level; }
void faceSetMode(bool table, uint8_t zoom) {
  tableMode = table;
  level = zoom < MAP_LEVEL_COUNT ? zoom : 0;
}

void faceToggleZoom() {
  level = (uint8_t)((level + 1) % MAP_LEVEL_COUNT);
  // Tracks are in pixels of the old scale: drop them and re-project from the
  // last fixes on the next frame, with no glide across the zoom.
  for (auto &t : tracks) t.used = false;
  seenSeq = 0;
}

void faceSetShift(int16_t dx, int16_t dy) {
  ox = dx;
  oy = dy;
}

// Wi-Fi setup: what to join on a phone, on black.
static void drawSetup(const Board &b) {
  gfx->clear(C_BG);
  const int16_t cx = (int16_t)(CX + ox);
  txtCenter(inter_label, "WI-FI SETUP", cx, (int16_t)(96 + oy), C_SOFT, 2);
  txtCenter(inter_unit, "On your phone, join", cx, (int16_t)(150 + oy), C_SOFT);
  txtCenter(inter_dest, b.setupAp, cx, (int16_t)(184 + oy), C_TEXT);
  txtCenter(inter_unit, "password", cx, (int16_t)(228 + oy), C_SOFT);
  txtCenter(inter_big, b.setupPass, cx, (int16_t)(284 + oy), C_TEXT);
  txtCenter(inter_unit, "A page opens to choose your Wi-Fi.", cx, (int16_t)(328 + oy), C_SOFT);
  txtCenter(inter_unit, "If not, open 192.168.4.1", cx, (int16_t)(352 + oy), C_DIM);
  if (b.setupNote[0])
    txtCenter(inter_unit, b.setupNote, cx, (int16_t)(392 + oy), C_AGE);
}

void faceDraw(const Board &b, double nowF, uint32_t ms) {
  if (b.state == NET_SETUP) { drawSetup(b); return; }
  const time_t now = (time_t)nowF;
  const bool clockOk = now > 1700000000;
  const uint32_t age = (b.fetchedAt && clockOk && now > b.fetchedAt)
                           ? (uint32_t)(now - b.fetchedAt) : 0;
  const bool stale = b.fetchedAt && age > STALE_S;
  const bool dead = !b.fetchedAt || age > DEAD_S;

  if (tableMode) {
    gfx->clear(C_BG);
  } else {
    blitMap();
    updateTracks(b, ms);
    drawStation(b, now, ms);
    if (clockOk) {
      drawEstimated(b, nowF, stale);
      drawTrams(b, ms, now, stale);
    }
    // Bands behind the text, over everything on the map: header at the top,
    // departures at the bottom. Vehicles fade under them; the text stays clear.
    scrim((int16_t)(36 + oy), (int16_t)(86 + oy), 60, 14);
    scrim((int16_t)(CY + 82 + oy), SCR_H, 70, 20);
  }

  char head[48], name[32];
  upperCopy(STOP_NAME, name, sizeof(name));
  if (clockOk) {
    struct tm t;
    localtime_r(&now, &t);
    snprintf(head, sizeof(head), "%s  %02d:%02d", name, t.tm_hour, t.tm_min);
  } else {
    strlcpy(head, name, sizeof(head));
  }
  txtCenter(inter_label, head, (int16_t)(CX + ox), (int16_t)(66 + oy), C_SOFT, 2);

  if (!clockOk || dead) {
    const char *msg = b.state == NET_CONNECTING ? "Connecting"
                      : b.state == NET_NO_TIME  ? "Setting clock"
                      : b.fetchedAt             ? "No data"
                                                : "Loading trams";
    const int16_t y = (int16_t)((tableMode ? CY + 6 : CY + 128) + oy);
    txtCenter(inter_dest, msg, (int16_t)(CX + ox), y, C_SOFT);
    if (b.state == NET_CONNECTING)
      txtCenter(inter_unit, "Hold BOOT to set up Wi-Fi", (int16_t)(CX + ox),
                (int16_t)(y + 26), C_DIM);
  } else if (tableMode) {
    drawTable(b, now, stale);
  } else {
    // Rows sit above the status line, the last one lowest.
    for (uint8_t r = 0; r < NUM_ROWS; r++) {
      const int16_t base = (int16_t)(CY + 162 - (NUM_ROWS - 1 - r) * 26);
      if (ROWS[r].count == 2) pairRow(b, ROWS[r], base, now, stale);
      else directionRow(b, ROWS[r].dirs[0], base, now, stale);
    }
  }
  statusLine(b, ms, age);
}
