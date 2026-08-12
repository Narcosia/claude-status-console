#include "motion.h"

#include <math.h>

// 64-entry half-sine lookup, 0..255. Built once so per-frame work is an
// integer table lookup rather than floating-point trig - the ring renders at
// ~33 fps alongside an HTTP server and a 466x466 UI.
static uint8_t SINE[64];
static bool sineReady = false;

static void ensureSine() {
  if (sineReady) return;
  for (int i = 0; i < 64; i++) {
    SINE[i] = (uint8_t)(127.5f * (1.0f - cosf(2.0f * (float)M_PI * i / 64.0f)));
  }
  sineReady = true;
}

MotionSpec motionFor(SessionState s) {
  switch (s) {
    // A quick comet: this session is on your turn right now.
    case ST_WORKING: return {true, 4};
    // A lazier comet. Even at a glance, without reading the colour, "running
    // without me" should look calmer than "working on my turn".
    case ST_BACKGROUND: return {true, 10};
    // Slow breath: finished, waiting on you, but nothing is blocked.
    case ST_INPUT: return {false, 1};
    // Fast pulse: something is actually blocked on you.
    case ST_ATTENTION: return {false, 3};
    default: return {false, 1};
  }
}

uint8_t motionSine(uint16_t i) {
  ensureSine();
  return SINE[i % 64];
}

uint8_t pulseLevel(uint32_t phase, uint16_t speed, uint16_t offset) {
  return motionSine((uint16_t)((phase * speed / 4 + offset) % 64));
}

uint16_t cometHead(uint16_t span, uint32_t phase, uint16_t offset,
                   uint16_t slow) {
  if (span == 0) return 0;
  if (slow == 0) slow = 1;
  return (uint16_t)((phase / slow + offset) % span);
}
