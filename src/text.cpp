#include "text.h"

#include <string.h>

#include "board_display.h"
#include "fonts/inter_label.h"
#include "fonts/inter_dest.h"
#include "fonts/inter_badge.h"
#include "fonts/inter_badge_s.h"
#include "fonts/inter_big.h"
#include "fonts/inter_unit.h"

uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t a) {
  if (a == 0) return bg;
  if (a == 255) return fg;
  const uint32_t ia = 255 - a;
  const uint32_t r = (((bg >> 11) & 0x1F) * ia + ((fg >> 11) & 0x1F) * a) / 255;
  const uint32_t g = (((bg >> 5) & 0x3F) * ia + ((fg >> 5) & 0x3F) * a) / 255;
  const uint32_t b = ((bg & 0x1F) * ia + (fg & 0x1F) * a) / 255;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// One UTF-8 codepoint, advancing *s past it. Malformed input yields U+FFFD
// and consumes one byte, so a bad string still terminates.
static uint32_t utf8Next(const char **s) {
  const uint8_t *p = (const uint8_t *)*s;
  const uint8_t c = p[0];
  uint32_t cp;
  uint8_t len;
  if (c < 0x80) { cp = c; len = 1; }
  else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
  else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
  else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
  else { *s += 1; return 0xFFFD; }
  for (uint8_t i = 1; i < len; i++) {
    if ((p[i] & 0xC0) != 0x80) { *s += 1; return 0xFFFD; }
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  *s += len;
  return cp;
}

static const AaGlyph *glyphFor(const AaFont &f, uint32_t cp) {
  // Printable ASCII is almost every glyph drawn, so it indexes directly.
  if (cp >= f.first && cp <= f.last) return &f.glyphs[cp - f.first];
  if (cp <= 0xFFFF && f.extra) {
    uint16_t lo = 0, hi = f.extra;
    while (lo < hi) {
      const uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
      if (f.codes[mid] < cp) lo = (uint16_t)(mid + 1);
      else if (f.codes[mid] > cp) hi = mid;
      else return &f.glyphs[f.last - f.first + 1 + mid];
    }
  }
  return &f.glyphs['?' - f.first];
}

int16_t txtWidth(const AaFont &f, const char *s, int16_t n, int8_t tracking) {
  if (!s) return 0;
  if (n < 0) n = (int16_t)strlen(s);
  const char *p = s;
  const char *const end = s + n;
  int32_t w = 0;
  bool any = false;
  while (p < end) {
    w += glyphFor(f, utf8Next(&p))->adv + tracking;
    any = true;
  }
  if (any) w -= tracking;
  return (int16_t)w;
}

static void drawGlyph(const AaFont &f, const AaGlyph *g, int16_t x,
                      int16_t baseline, uint16_t col) {
  if (g->w == 0 || g->h == 0) return;
  uint16_t *fb = gfx->fb();
  if (!fb) return;
  const int16_t x0 = (int16_t)(x + g->xo);
  const int16_t y0 = (int16_t)(baseline + g->yo);
  const uint8_t *a = f.alpha + g->off;
  for (int16_t j = 0; j < g->h; j++) {
    const int16_t y = (int16_t)(y0 + j);
    if (y < 0 || y >= SCR_H) { a += g->w; continue; }
    uint16_t *row = fb + (int32_t)y * SCR_W;
    for (int16_t i = 0; i < g->w; i++, a++) {
      const int16_t px = (int16_t)(x0 + i);
      if (px < 0 || px >= SCR_W || *a == 0) continue;
      row[px] = blend565(row[px], col, *a);
    }
  }
}

void txtDraw(const AaFont &f, const char *s, int16_t x, int16_t baseline,
             uint16_t col, int16_t n, int8_t tracking) {
  if (!s) return;
  if (n < 0) n = (int16_t)strlen(s);
  const char *p = s;
  const char *const end = s + n;
  while (p < end) {
    const AaGlyph *g = glyphFor(f, utf8Next(&p));
    drawGlyph(f, g, x, baseline, col);
    x = (int16_t)(x + g->adv + tracking);
  }
}

void txtCenter(const AaFont &f, const char *s, int16_t cx, int16_t baseline,
               uint16_t col, int8_t tracking) {
  const int16_t w = txtWidth(f, s, -1, tracking);
  txtDraw(f, s, (int16_t)(cx - w / 2), baseline, col, -1, tracking);
}

void txtRight(const AaFont &f, const char *s, int16_t rightX, int16_t baseline,
              uint16_t col, int8_t tracking) {
  const int16_t w = txtWidth(f, s, -1, tracking);
  txtDraw(f, s, (int16_t)(rightX - w), baseline, col, -1, tracking);
}

uint16_t txtFitLine(const AaFont &f, const char *s, int16_t maxW, const char **next) {
  const char *lineStart = s;
  const char *lastBreak = nullptr;
  const char *q = s;
  while (*q) {
    const char *wordEnd = q;
    while (*wordEnd && *wordEnd != ' ') wordEnd++;
    if (txtWidth(f, lineStart, (int16_t)(wordEnd - lineStart)) > maxW && lastBreak) break;
    lastBreak = wordEnd;
    q = wordEnd;
    while (*q == ' ') q++;
    if (!*wordEnd) break;
  }
  const char *lineEnd = lastBreak ? lastBreak : q;
  if (lineEnd == lineStart) {   // one oversized word: hard cut at its end
    while (*lineEnd && *lineEnd != ' ') lineEnd++;
  }
  const char *n = lineEnd;
  while (*n == ' ') n++;
  *next = n;
  return (uint16_t)(lineEnd - lineStart);
}
