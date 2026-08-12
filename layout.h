// How the ring is divided between sessions.
//
// Shared by the LED ring and the screen so the two cannot drift apart: an arc
// on the display covers exactly the LEDs its session owns, which is what makes
// the screen and the ring read as one object rather than two displays that
// happen to agree.

#pragma once

#include <Arduino.h>

// An arc's LED index range, [lo, hi).
struct ArcSpan {
  uint16_t lo, hi;
};

// Divide `ledCount` pixels among `count` sessions.
//
// One dark LED is left after each arc so two same-coloured neighbours cannot
// read as a single segment. A lone session needs no separator, and if there
// are so many sessions that gaps would starve the arcs, the gaps are dropped
// rather than the sessions. Any remainder is spread over the first arcs.
//
// Returns the number of spans written, which is min(count, ledCount, maxOut).
size_t computeArcs(size_t count, uint16_t ledCount, ArcSpan *out, size_t maxOut);
