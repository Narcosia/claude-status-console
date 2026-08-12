// The 466x466 round display: one arc per session, at the same angle as that
// session's arc on the physical ring, around a centre summary.
//
// The screen is not a second, richer display of the same data - it is the same
// display, at higher resolution. An arc here covers exactly the LEDs its
// session owns and animates on the same phase, so the two surfaces read as one
// object. What the screen adds is the project name, which is the one thing an
// LED arc cannot tell you.

#pragma once

#include <lvgl.h>

#include "ring.h"
#include "sessions.h"

// Where LED 0 sits, and which way the chain runs from it. Together these are
// what make a screen arc point at its physical arc; get them wrong and the
// arcs are rotated or mirrored, which is subtle enough to look like a bug
// somewhere else entirely.
//
// Angles are LVGL degrees: 0 is 3 o'clock, increasing clockwise.
//
//   12 o'clock 270    3 o'clock 0    6 o'clock 90    9 o'clock 180
//
// Measured with POST /ringzero, which marks LED 0 in white with a fading trail
// showing the direction of increasing index.
//
// This ring: LED 0 sits at the wire entry, at 6 o'clock.
#ifndef RING_ZERO_DEG
#define RING_ZERO_DEG 90
#endif

// 1 if LED indices increase clockwise when viewed from the front, 0 if they
// run anticlockwise. The trail in the zero marker shows which.
#ifndef RING_CLOCKWISE
#define RING_CLOCKWISE 1
#endif

void uiInit(const Palette &palette, uint16_t ledCount);

// Draw one frame from a session snapshot. `phase` is the same counter the ring
// renders from, which is what keeps the two in step.
void uiUpdate(const Session *sessions, size_t n, uint32_t phase,
              uint32_t lingerMs);

// Shown small at the bottom of the screen, so the address the hooks need is
// readable off the device itself rather than only over serial.
void uiSetNetwork(const char *text);
