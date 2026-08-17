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

// Stargate event horizon behind the centre summary: concentric ripples
// travelling outward, with nine chevrons around the inner edge that lock as
// sessions appear.
//
// It sits BEHIND the text and stays dim on purpose. The centre is where the
// state is read, and decoration that costs legibility is a bad trade on a
// device whose entire job is to be readable at a glance.
//
// The chevrons take the colour of the most urgent session rather than the
// canonical Stargate amber: warm means "wants you" everywhere else on this
// device, and a decorative amber that meant nothing would break the one rule
// the whole colour scheme rests on.
//
// Currently 0: the effect is preserved but compiled out. See
// docs/DISPLAY-PERFORMANCE.md for why, and for what to try before turning it
// back on. Set to 1 to re-enable.
#ifndef STARGATE
#define STARGATE 0
#endif

void uiInit(const Palette &palette, uint16_t ledCount);

// Draw one frame from a session snapshot. `phase` is the same counter the ring
// renders from, which is what keeps the two in step.
void uiUpdate(const Session *sessions, size_t n, uint32_t phase,
              uint32_t lingerMs);

// Shown small at the bottom of the screen, so the address the hooks need is
// readable off the device itself rather than only over serial.
void uiSetNetwork(const char *text);

// Fire the unstable vortex by hand. Called automatically at boot and whenever
// a new session appears; exposed so POST /kawoosh can trigger it too, which is
// the only way to watch it without waiting for a session to start.
void uiKawoosh();

// Swap between the status and lights pages. Called from the touch driver,
// which detects swipes itself - LVGL's gesture engine assumes a continuous
// stream of coordinates, and this panel reports in bursts around interrupts.
void uiTogglePage();

// True briefly after a swipe, so a press that was part of a drag does not also
// fire whatever control it started or ended on.
bool uiSwipeActive();

// Render every page once at boot, reporting LVGL pool usage around each, and
// return to the status page.
//
// This exists because a page that renders only when a finger lands on it can
// only be tested by a finger. A shadow on an oversized object exhausted the
// LVGL pool and hung the device inside LV_ASSERT_MALLOC's while(1) - and
// compile, flash, hash-verify and an HTTP health probe all passed while it sat
// one touch away from locking up. This forces the same draw at boot, over
// serial, where it can be seen without the hardware in hand.
void uiSelfTest();
