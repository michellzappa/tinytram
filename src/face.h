#pragma once

#include <Arduino.h>

#include "departures.h"

// Draws one frame into the canvas. nowF is epoch seconds with fraction, so
// the dial moves smoothly; ms is millis() for animation.
void faceDraw(const Board &b, double nowF, uint32_t ms);
// Burn-in orbit offset in pixels.
void faceSetShift(int16_t dx, int16_t dy);
// Switches the map between its zoom levels (MAP_LEVELS).
void faceToggleZoom();
// Map or table mode; table mode is plain rows on black.
void faceToggleTable();
bool faceTableMode();
uint8_t faceZoom();
void faceSetMode(bool table, uint8_t zoom);
