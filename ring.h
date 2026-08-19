// Renders session state onto the WS2812B ring. A C++ port of ring.py.
//
// The ring is divided into equal arcs, one per active session, ordered by when
// each session was first seen so an arc does not jump around mid-run.
//
// Neighbouring arcs are separated two ways, because one cue is not enough: a
// dark LED sits between them (visible even in a still photo), and each arc's
// animation is staggered around the cycle so two adjacent arcs pulse in
// antiphase rather than breathing as one.
//
//   working    brighter comet sweeping the arc (on your turn)
//   background slower comet (agents running - wants nothing from you)
//   input      slow pulse   <- turn finished, awaiting your prompt
//   attention  fast pulse   <- permission prompt / needs input now
//   done       fades out over the linger window
//
// Motion carries the same information independently of hue: comets mean
// "leave it alone", pulses mean "it wants you", and faster means more urgent
// within each pair. The screen uses the same palette and the same rule.

#pragma once

#include <Arduino.h>

#include "layout.h"
#include "sessions.h"

// Driven straight from the RMT peripheral rather than through
// Adafruit_NeoPixel, for two reasons found the hard way:
//
//   1. On rmtInit() failure its espShow() returns while still holding its
//      mutex, so every later show() blocks 50 ms and silently does nothing.
//      A dead ring reports itself as a healthy one.
//   2. It hardcodes RMT_MEM_NUM_BLOCKS_1 - a 48-symbol buffer for a 576-symbol
//      frame - so transmission depends on a refill interrupt landing on time,
//      every frame. Beside WiFi and a 466x466 LVGL flush that is not a safe
//      assumption, and an underrun shows up as a frozen or garbled ring.
//
// Here the buffer is sized generously and every failure is reported.

struct RGB {
  uint8_t r, g, b;
};

// Indexed by SessionState (ST_WORKING..ST_DONE).
struct Palette {
  RGB colour[5];
};

// Whatever the theme, one rule holds: states that want you are WARM, states
// that do not are COOL. That split has to survive a glance from across the
// room, long before you have read the individual hue.
//
// DONE is a white fade in every theme: BACKGROUND owns green, and it is the
// far more common and longer-lived state, so sharing a colour would have
// reintroduced exactly the ambiguity the palette exists to remove.
extern const Palette PALETTE_CLASSIC;
extern const Palette PALETTE_NEON;
extern const Palette PALETTE_VAPOR;

// Accepts a palette name, falling back to classic with a warning. A typo in
// config should dim the lights, not take an unattended device's server down.
const Palette &resolvePalette(const char *name);

// A millis() deadline. While it is in the future the ring draws a solid test
// pattern instead of session arcs, at a brightness that cannot be mistaken for
// off. Set by POST /ringtest.
//
// This exists to answer one question quickly: if the test pattern lights, the
// wiring and the WS2812B chain are fine and the fault is in the arc rendering;
// if it does not, the signal is not reaching the ring at all. Guessing between
// those two costs far more than the endpoint does.
extern volatile uint32_t g_ringTestUntil;

// What to draw while g_ringTestUntil is in the future.
enum RingOverride : uint8_t {
  RING_OVERRIDE_TEST = 0,  // solid colour cycle - "is anything alive"
  RING_OVERRIDE_ZERO = 1,  // index marker - "where is LED 0, and which way"
  RING_OVERRIDE_SCAN = 2,  // pin scan - "which GPIO is Din actually in"
  RING_OVERRIDE_HIGH = 3,  // hold one pin high - "which HOLE is that GPIO in"
};
extern volatile uint8_t g_ringOverride;

// Which GPIO RING_OVERRIDE_HIGH holds high. Set by POST /pinhigh?gpio=NN.
//
// The scan answers "which pin is the ring on" by lighting it. This answers the
// question you have when there is no ring to light: which physical hole is a
// given GPIO? A WS2812B data stream averages ~0.05 V and is unreadable on a
// multimeter; a pin held steady at 3.3 V is not.
extern volatile uint8_t g_ringHighPin;

class Ring {
 public:
  Ring(uint8_t pin, uint16_t count, uint8_t brightness, const char *theme);

  // Returns false if the RMT channel or its buffers could not be claimed, and
  // says why on the serial console. Call before anything allocates large DMA
  // buffers - see the note in the sketch's setup().
  bool begin();
  void clear();

  bool ready() const { return _ready; }

  // Draw one frame. `sessions` is a snapshot, already ordered by first-seen.
  void render(const Session *sessions, size_t n, uint32_t phase,
              uint32_t lingerMs);

  // Every LED lit, cycling red -> green -> blue about once a second. Ignores
  // the configured brightness in favour of a fixed level that is unambiguous
  // in a lit room but still only ~110 mA across 24 LEDs.
  void testPattern(uint32_t phase);

  // LED 0 bright white, the next two fading away from it, and dim blue marks
  // at each quarter. Answers where index 0 sits on the clock face and which
  // way the indices run - both needed to set RING_ZERO_DEG so the screen arcs
  // point at the real ones.
  void zeroMarker();

  // Drives each candidate expansion GPIO in turn, in its own colour, ~2 s
  // each: GPIO16 red, GPIO17 green, GPIO18 blue. The colour the ring shows
  // identifies which header pin Din is actually seated in.
  //
  // Driving all three at once - as the first probe sketch did - lights the
  // ring without revealing which pin did it, which is precisely the question
  // when the ring works under one firmware and not another.
  void pinScan(uint32_t phase);

  // Hold one GPIO steadily high, releasing the RMT channel first so the two
  // are not fighting over the pad. Measure the holes with a multimeter: the
  // one reading 3.3 V is that GPIO.
  //
  // Idempotent - safe to call every frame while the override is active.
  void holdPinHigh(uint8_t pin);

  // Give the pad back to the RMT driver after a hold.
  void releasePin();

 private:
  void paintComet(uint16_t lo, uint16_t hi, RGB c, uint32_t phase,
                  uint16_t offset, uint16_t slow);
  void paintPulse(uint16_t lo, uint16_t hi, RGB c, uint32_t phase,
                  uint16_t speed, uint16_t offset);
  void paintDone(uint16_t lo, uint16_t hi, uint8_t fade);
  void paintHeartbeat(uint32_t phase);
  void set(uint16_t i, RGB c, uint8_t level);
  void wipe();               // all pixels to black, without transmitting
  void show();               // encode and transmit on the configured pin
  void transmit(uint8_t pin);  // encode and transmit on an arbitrary pin
  bool usePin(uint8_t pin);    // (re)claim the RMT channel for this pin

  uint8_t _pin;
  uint16_t _n;
  uint8_t _peak;
  const Palette *_pal;
  uint8_t *_pixels;      // GRB, 3 bytes per LED
  rmt_data_t *_symbols;  // 24 symbols per LED
  bool _ready;
  uint8_t _activePin;    // pin the RMT channel is currently bound to, 0xFF none
  uint32_t _failures;    // transmit failures, reported rate-limited
};
