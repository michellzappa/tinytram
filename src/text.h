// Anti-aliased text straight into the PSRAM framebuffer.
#pragma once

#include <Arduino.h>

#include "aafont.h"

extern const AaFont inter_label;    // 14px 600, caps labels
extern const AaFont inter_dest;     // 21px 500, hero destination
extern const AaFont inter_badge;    // 19px 700, line number
extern const AaFont inter_badge_s;  // 13px 700, small line number
extern const AaFont inter_big;      // 50px 600, hero minutes
extern const AaFont inter_unit;     // 16px 500, units, followers, status

uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t a);

int16_t txtWidth(const AaFont &f, const char *s, int16_t n = -1, int8_t tracking = 0);
void txtDraw(const AaFont &f, const char *s, int16_t x, int16_t baseline,
             uint16_t col, int16_t n = -1, int8_t tracking = 0);
void txtCenter(const AaFont &f, const char *s, int16_t cx, int16_t baseline,
               uint16_t col, int8_t tracking = 0);
void txtRight(const AaFont &f, const char *s, int16_t rightX, int16_t baseline,
              uint16_t col, int8_t tracking = 0);

// Longest prefix of s (breaking at spaces) that fits in maxW. Returns the
// length; *next is where the following line starts.
uint16_t txtFitLine(const AaFont &f, const char *s, int16_t maxW, const char **next);
