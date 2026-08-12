// The animation vocabulary, shared by the LED ring and the screen.
//
// Motion carries the same information as colour, independently: comets mean
// "leave it alone", pulses mean "it wants you", and faster means more urgent
// within each pair. That rule is what keeps the vapor theme viable, since its
// pink `input` and pink-red `attention` are close enough that the pulse rate
// does most of the work.
//
// Both renderers read their timing from here so a session's arc on screen
// breathes in step with the same session's arc on the ring. Two surfaces
// animating the same state at slightly different rates would read as two
// different states.

#pragma once

#include <Arduino.h>

#include "sessions.h"

struct MotionSpec {
  bool comet;      // true: sweeping head. false: whole-arc pulse.
  uint16_t rate;   // comet: phase divisor (larger is slower).
                   // pulse: phase multiplier (larger is faster).
};

MotionSpec motionFor(SessionState s);

// One entry of the 64-step half-sine, 0..255.
uint8_t motionSine(uint16_t i);

// Pulse brightness at this phase, 0..255, before any floor or scaling.
uint8_t pulseLevel(uint32_t phase, uint16_t speed, uint16_t offset);

// Position of a comet's head within an arc, as an offset in [0, span).
uint16_t cometHead(uint16_t span, uint32_t phase, uint16_t offset, uint16_t slow);
