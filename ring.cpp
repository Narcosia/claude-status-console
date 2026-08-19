#include "ring.h"

#include <string.h>

#include "motion.h"

const Palette PALETTE_CLASSIC = {{
    {0, 40, 255},     // working    blue
    {0, 255, 60},     // background green
    {255, 140, 0},    // input      amber
    {255, 0, 0},      // attention  red
    {255, 255, 255},  // done       white
}};

// Cyan and hot pink carry the theme because working and input are the two
// states you see most. Green and red stay as accents rather than being forced
// into the scheme: background needs to sit apart from working, and red is the
// one colour nobody has to learn.
const Palette PALETTE_NEON = {{
    {0, 220, 255},    // cyan
    {0, 255, 120},    // spring green
    {255, 0, 130},    // hot pink
    {255, 20, 40},    // red
    {255, 255, 255},
}};

// Everything in the pink/cyan family, for when the ring is decor as much as
// instrument. Less legible - attention leans on its fast pulse to stand out
// from input rather than on hue.
const Palette PALETTE_VAPOR = {{
    {0, 220, 255},    // cyan
    {120, 60, 255},   // violet
    {255, 0, 160},    // hot pink
    {255, 40, 90},    // pink-red
    {255, 255, 255},
}};

volatile uint32_t g_ringTestUntil = 0;
volatile uint8_t g_ringOverride = RING_OVERRIDE_TEST;
volatile uint8_t g_ringHighPin = 16;

// WS2812B bit timing, in ticks of the 10 MHz RMT clock set in begin(), so one
// tick is 100 ns. Spec allows +/-150 ns on each, and a bit period of ~1.25 us.
//
//   0 bit: 400 ns high, 800 ns low
//   1 bit: 800 ns high, 400 ns low
static const uint16_t T0H = 4, T0L = 8, T1H = 8, T1L = 4;

// 24 bits per LED, one RMT symbol per bit.
static const uint16_t SYMBOLS_PER_LED = 24;

const Palette &resolvePalette(const char *name) {
  if (!name) return PALETTE_CLASSIC;
  if (strcmp(name, "vapor") == 0) return PALETTE_VAPOR;
  if (strcmp(name, "neon") == 0) return PALETTE_NEON;
  if (strcmp(name, "classic") == 0) return PALETTE_CLASSIC;
  Serial.printf("ring: unknown theme '%s', using classic\n", name);
  return PALETTE_CLASSIC;
}

static inline RGB scale(RGB c, uint16_t level) {
  RGB out;
  out.r = (uint8_t)((uint16_t)c.r * level / 255);
  out.g = (uint8_t)((uint16_t)c.g * level / 255);
  out.b = (uint8_t)((uint16_t)c.b * level / 255);
  return out;
}

Ring::Ring(uint8_t pin, uint16_t count, uint8_t brightness, const char *theme)
    : _pin(pin),
      _n(count),
      _peak(brightness),
      _pal(&resolvePalette(theme)),
      _pixels(nullptr),
      _symbols(nullptr),
      _ready(false),
      _activePin(0xFF),
      _failures(0) {}

bool Ring::usePin(uint8_t pin) {
  if (_activePin == pin) return true;

  if (_activePin != 0xFF) rmtDeinit(_activePin);
  if (!rmtInit(pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_4, 10000000)) {
    Serial.printf("ring: rmtInit failed on GPIO%u\n", pin);
    _activePin = 0xFF;
    return false;
  }
  // Idle low between frames. WS2812Bs latch on a low period over ~50 us, and
  // the 30 ms between frames supplies that comfortably.
  rmtSetEOT(pin, 0);
  _activePin = pin;
  return true;
}

bool Ring::begin() {
  _pixels = (uint8_t *)calloc(_n * 3, 1);
  _symbols = (rmt_data_t *)malloc((size_t)_n * SYMBOLS_PER_LED * sizeof(rmt_data_t));
  if (!_pixels || !_symbols) {
    Serial.println("ring: buffer allocation failed");
    return false;
  }

  // Four memory blocks rather than the single block Adafruit_NeoPixel uses.
  // A frame is 576 symbols and a block holds 48, so the driver refills from an
  // interrupt mid-transmission either way - but four blocks give that
  // interrupt four times the slack, which matters with WiFi and a full-screen
  // LVGL flush competing for the CPU.
  if (!usePin(_pin)) {
    Serial.printf("ring: could not claim GPIO%u - ring will stay dark\n", _pin);
    return false;
  }

  _ready = true;
  Serial.printf("ring: RMT ready on GPIO%u, %u LEDs\n", _pin, _n);
  clear();
  return true;
}

void Ring::wipe() {
  if (_pixels) memset(_pixels, 0, _n * 3);
}

void Ring::show() { transmit(_pin); }

void Ring::transmit(uint8_t pin) {
  if (!_ready) return;
  if (!usePin(pin)) return;

  rmt_data_t *s = _symbols;
  for (uint16_t i = 0; i < _n * 3; i++) {
    uint8_t byte = _pixels[i];
    for (int bit = 7; bit >= 0; bit--) {
      bool one = byte & (1 << bit);
      s->level0 = 1;
      s->duration0 = one ? T1H : T0H;
      s->level1 = 0;
      s->duration1 = one ? T1L : T0L;
      s++;
    }
  }

  // A bounded timeout, not RMT_WAIT_FOR_EVER: a frame is under a millisecond,
  // so anything approaching 50 ms means the peripheral is wedged, and blocking
  // the render task forever would take the heartbeat down with it - removing
  // the very signal that says the ring is in trouble.
  if (!rmtWrite(pin, _symbols, (size_t)_n * SYMBOLS_PER_LED, 50)) {
    _failures++;
    // Rate-limited: at 33 fps an unconditional print would bury everything.
    if (_failures == 1 || _failures % 100 == 0) {
      Serial.printf("ring: rmtWrite failed on GPIO%u (%lu so far)\n", pin,
                    (unsigned long)_failures);
    }
  }
}

void Ring::pinScan(uint32_t phase) {
  static const uint8_t PINS[3] = {16, 17, 18};
  static const RGB COLS[3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
  // Hole numbers per board.h, which is measured. These strings used to carry
  // Waveshare's ordering (8/6/7) and so contradicted the very finding this
  // scan produced - a diagnostic printing a map it had itself disproved.
  static const char *NAMES[3] = {"GPIO16 (H2 pin 6) RED",
                                 "GPIO17 (H2 pin 7) GREEN",
                                 "GPIO18 (H2 pin 8) BLUE"};

  // ~2 s per pin at 33 fps.
  uint8_t idx = (phase / 66) % 3;

  static uint8_t announced = 0xFF;
  if (idx != announced) {
    announced = idx;
    Serial.printf("ring scan: driving %s\n", NAMES[idx]);
  }

  for (uint16_t i = 0; i < _n; i++) set(i, COLS[idx], 60);
  transmit(PINS[idx]);
}

void Ring::holdPinHigh(uint8_t pin) {
  static uint8_t held = 0xFF;
  if (held == pin) return;          // already driving it; nothing to redo

  // The RMT owns the pad until it is deinited. Without this the two fight and
  // the level a meter sees depends on which spoke last.
  releasePin();
  pinMode(pin, OUTPUT);
  digitalWrite(pin, HIGH);
  held = pin;
  Serial.printf("pin hold: GPIO%u driven HIGH (3.3 V) - probe the holes\n", pin);
}

void Ring::releasePin() {
  if (_activePin == 0xFF) return;
  rmtDeinit(_activePin);
  _activePin = 0xFF;
}

void Ring::clear() {
  wipe();
  show();
}

void Ring::set(uint16_t i, RGB c, uint8_t level) {
  if (!_pixels || i >= _n) return;
  RGB s = scale(c, level);
  uint8_t *p = &_pixels[i * 3];
  p[0] = s.g;  // WS2812B wants GRB order
  p[1] = s.r;
  p[2] = s.b;
}

void Ring::paintComet(uint16_t lo, uint16_t hi, RGB c, uint32_t phase,
                      uint16_t offset, uint16_t slow) {
  uint16_t span = hi - lo;
  if (span == 0) return;
  for (uint16_t i = lo; i < hi; i++) set(i, c, _peak / 5);

  uint16_t headOff = cometHead(span, phase, offset, slow);
  set(lo + headOff, c, _peak);
  // Short tail, so direction of travel reads clearly.
  set(lo + (uint16_t)((headOff + span - 1) % span), c, _peak / 2);
}

void Ring::paintPulse(uint16_t lo, uint16_t hi, RGB c, uint32_t phase,
                      uint16_t speed, uint16_t offset) {
  uint16_t level = (uint16_t)_peak * pulseLevel(phase, speed, offset) / 255;
  // Floor keeps the arc readable at the bottom of the pulse.
  if (level < (uint16_t)(_peak / 6)) level = _peak / 6;
  for (uint16_t i = lo; i < hi; i++) set(i, c, (uint8_t)level);
}

void Ring::paintDone(uint16_t lo, uint16_t hi, uint8_t fade) {
  uint16_t level = (uint16_t)_peak * fade / 255;
  for (uint16_t i = lo; i < hi; i++) set(i, _pal->colour[ST_DONE], (uint8_t)level);
}

void Ring::paintHeartbeat(uint32_t phase) {
  // With no sessions, a single very dim pixel creeps around the ring: a
  // heartbeat that distinguishes "idle" from "powered off" at a glance.
  uint8_t level = _peak / 8;
  if (level < 1) level = 1;
  set((phase / 12) % _n, _pal->colour[ST_WORKING], level);
}

void Ring::testPattern(uint32_t phase) {
  // ~60/255 on one channel: bright enough to be obvious, and at 24 LEDs still
  // well under what VBUS can spare beside the AMOLED.
  static const RGB STEPS[3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
  RGB c = STEPS[(phase / 33) % 3];
  for (uint16_t i = 0; i < _n; i++) set(i, c, 60);
  show();
}

void Ring::zeroMarker() {
  wipe();

  // LED 0, unmistakable.
  set(0, {255, 255, 255}, 110);
  // The next two indices, fading. The direction the trail runs is the answer
  // to "do indices go clockwise or anticlockwise", which decides whether the
  // screen mapping needs mirroring.
  if (_n > 1) set(1, {255, 255, 255}, 35);
  if (_n > 2) set(2, {255, 255, 255}, 12);

  // Quarter marks, so the position can be read off without counting 24 LEDs.
  uint16_t step = _n / 4;
  if (step) {
    for (uint16_t q = step; q < _n; q += step) set(q, {0, 90, 255}, 30);
  }

  show();
}

void Ring::render(const Session *sessions, size_t n, uint32_t phase,
                  uint32_t lingerMs) {
  wipe();

  if (n == 0) {
    paintHeartbeat(phase);
    show();
    return;
  }

  ArcSpan spans[MAX_SESSIONS];
  size_t count = computeArcs(n, _n, spans, MAX_SESSIONS);

  for (size_t idx = 0; idx < count; idx++) {
    const Session &s = sessions[idx];
    uint16_t lo = spans[idx].lo;
    uint16_t hi = spans[idx].hi;
    uint16_t span = hi - lo;

    // Second, motion-based cue: stagger each arc's animation around the cycle
    // so neighbours are never in step.
    uint16_t phaseOff = (uint16_t)(idx * 64 / count);
    uint16_t cometOff = (uint16_t)(idx * span / count);

    SessionState st = displayState(s);
    if (st == ST_DONE) {
      uint32_t elapsed = s.hasEnded ? (millis() - s.ended) : 0;
      uint32_t linger = lingerMs ? lingerMs : 1;
      int32_t fade = 255 - (int32_t)(elapsed * 255 / linger);
      paintDone(lo, hi, fade < 0 ? 0 : (uint8_t)fade);
    } else {
      MotionSpec m = motionFor(st);
      if (m.comet) {
        paintComet(lo, hi, _pal->colour[st], phase, cometOff, m.rate);
      } else {
        paintPulse(lo, hi, _pal->colour[st], phase, m.rate, phaseOff);
      }
    }
  }

  show();
}
