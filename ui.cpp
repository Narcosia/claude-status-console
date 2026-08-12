#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "layout.h"
#include "motion.h"

static const lv_coord_t SCREEN = 466;
static const lv_coord_t CX = SCREEN / 2;
static const lv_coord_t CY = SCREEN / 2;

// The arc band sits just inside the bezel. ARC_SIZE is the outer diameter of
// the arc objects; LVGL centres the stroke on that circle, so the band runs
// from ARC_SIZE/2 - ARC_W to ARC_SIZE/2.
static const lv_coord_t ARC_SIZE = 448;
static const lv_coord_t ARC_W = 26;
static const float BAND_OUTER = ARC_SIZE / 2.0f + 6.0f;   // touch slop
static const float BAND_INNER = ARC_SIZE / 2.0f - ARC_W - 12.0f;

// Floors, so a pulsing arc never disappears completely and a dim comet body
// still reads as a claimed segment of ring.
static const lv_opa_t PULSE_FLOOR = 42;   // 255/6, matching the ring
static const lv_opa_t COMET_BODY = 55;

// Longer than the old 8 s: there is now a prompt line to actually read, not
// just a state word to glance at.
static const uint32_t DETAIL_HOLD_MS = 15000;

static lv_obj_t *arcBase[MAX_SESSIONS];
static lv_obj_t *arcHead[MAX_SESSIONS];
static lv_obj_t *lblCount;
static lv_obj_t *lblWord;
static lv_obj_t *lblBreak;
static lv_obj_t *lblNet;

static lv_obj_t *detail;
static lv_obj_t *lblDetailName;
static lv_obj_t *lblDetailPath;
static lv_obj_t *lblDetailTopic;
static lv_obj_t *lblDetailNow;
static lv_obj_t *lblDetailState;
static lv_obj_t *lblDetailMeta;

static const Palette *g_pal = nullptr;
static uint16_t g_leds = 24;

// The last snapshot drawn, kept so the touch handler can answer "which session
// is at this angle" without reaching back into the registry.
static Session g_shown[MAX_SESSIONS];
static size_t g_shownN = 0;
static ArcSpan g_spans[MAX_SESSIONS];
static size_t g_spanN = 0;

// The open detail card is remembered by session id, not by arc position.
// Positions shift as sessions come and go, and a card that silently re-points
// at a different session is worse than one that closes.
static char g_detailSid[SID_LEN] = {0};
static bool g_detailOpen = false;
static uint32_t g_detailUntil = 0;

// --- Stargate centre --------------------------------------------------------
//
// Seven concentric ripples and nine chevrons. The ripples are thin outlines
// rather than filled discs: cheap to redraw, and they do not wash out the text
// sitting on top of them.
#if STARGATE
// Twelve rings rather than a handful: the kawoosh is an expanding front, and a
// coarse set of radii makes it read as steps rather than a surge.
// Eight stacked discs: enough for a smooth radial ramp, few enough that the
// overdraw stays affordable. The outermost is 298 px, which keeps the pool
// clear of the session band at radius 198 so the arcs are not dragged into
// every centre redraw.
static const size_t GATE_RINGS = 6;
static const size_t CHEVRONS = 9;
static const lv_coord_t GATE_INNER = 60;     // diameter of the core disc
static const lv_coord_t GATE_STEP = 46;      // spacing, so the outermost is 290
static const lv_coord_t CHEVRON_SIZE = 372;  // just inside the session band
// Per-disc, and they stack. At ~100 each over black, six overlapping layers
// composite to a nearly solid core and a single layer at the rim - which is
// the filled pool with a hot centre, rather than the faint outlines this
// started as. The shimmer rides between MIN and MAX.
static const lv_opa_t GATE_MIN = 74;
static const lv_opa_t GATE_MAX = 132;

// Chevron orange, straight off the prop. This is decoration and not status -
// everywhere else on this device warm means "wants you", and that meaning now
// lives entirely on the arcs and the ring, which is where it is most visible
// anyway. The centre count keeps its urgency colour when the gate is dormant.
static const uint32_t CHEVRON_LIT = 0xFF6A18;
static const uint32_t CHEVRON_DARK = 0x241a12;

// The unstable vortex: erupts from the centre, flushes outward, retracts, then
// settles. Timed in one place so the phases below stay legible as fractions.
static const uint32_t KAWOOSH_MS = 1150;

static lv_obj_t *gateRipple[GATE_RINGS];
static lv_obj_t *chevron[CHEVRONS];

static uint32_t g_burstStart = 0;
static bool g_bursting = false;
static size_t g_prevSessions = 0;
static bool g_gateSeeded = false;
#endif

static inline lv_color_t colourOf(SessionState s) {
  RGB c = g_pal->colour[s];
  return lv_color_make(c.r, c.g, c.b);
}

// LED index -> LVGL degrees, wrapped into [0, 360).
static uint16_t ledToDeg(float led) {
  float step = led * 360.0f / (float)g_leds;
  float d = RING_ZERO_DEG + (RING_CLOCKWISE ? step : -step);
  while (d < 0) d += 360.0f;
  while (d >= 360.0f) d -= 360.0f;
  return (uint16_t)(d + 0.5f);
}

// Every LVGL write below goes through a cache, because the two kinds of change
// cost wildly different amounts:
//
//   angles -> LVGL invalidates only the sector that actually moved: cheap
//   style  -> LVGL invalidates the object's whole bounding box, and these arcs
//             are 448x448, so one colour write costs a full-screen redraw
//
// Rewriting colour and opacity on every arc every frame - which is what this
// did originally - meant eight full-screen invalidations per frame whether or
// not the picture had changed. Measured at 85 ms a pass with four sessions,
// about 11 fps, before the event horizon was added at all.
struct ArcCache {
  bool init;
  uint16_t start, end;
  lv_color_t colour;
  lv_opa_t opa;
};
static ArcCache baseCache[MAX_SESSIONS];
static ArcCache headCache[MAX_SESSIONS];

static void setArcStyle(lv_obj_t *arc, ArcCache &c, lv_color_t colour,
                        lv_opa_t opa) {
  if (!c.init || colour.full != c.colour.full) {
    lv_obj_set_style_arc_color(arc, colour, LV_PART_INDICATOR);
    c.colour = colour;
  }
  if (!c.init || opa != c.opa) {
    lv_obj_set_style_arc_opa(arc, opa, LV_PART_INDICATOR);
    c.opa = opa;
  }
  c.init = true;
}

// LVGL always fills an arc clockwise from start to end, so an anticlockwise
// LED chain needs its endpoints swapped or every arc is drawn as its
// complement - the gap instead of the segment.
static void setArcSpan(lv_obj_t *arc, ArcCache &c, float loLed, float hiLed) {
  uint16_t a = ledToDeg(loLed), b = ledToDeg(hiLed);
  if (!RING_CLOCKWISE) {
    uint16_t t = a;
    a = b;
    b = t;
  }
  if (!c.init || a != c.start || b != c.end) {
    lv_arc_set_angles(arc, a, b);
    c.start = a;
    c.end = b;
  }
}

// Pulse brightness quantised to 16 levels. Invisible as a step on a slow
// breath, and it collapses a stream of one-unit opacity changes - each of them
// a full-screen invalidation - into a handful per cycle.
static inline lv_opa_t quantise(uint16_t v) {
  if (v > 255) v = 255;
  return (lv_opa_t)((v / 16) * 16);
}

static const char *stateWord(SessionState s) {
  switch (s) {
    case ST_WORKING: return "working";
    case ST_BACKGROUND: return "agents running";
    case ST_INPUT: return "waiting for you";
    case ST_ATTENTION: return "needs you now";
    case ST_DONE: return "finished";
    default: return "";
  }
}

// Compact age: 12s, 4m, 2h.
static void formatAge(uint32_t ms, char *out, size_t n) {
  uint32_t s = ms / 1000;
  if (s < 60) {
    snprintf(out, n, "%us", (unsigned)s);
  } else if (s < 3600) {
    snprintf(out, n, "%um", (unsigned)(s / 60));
  } else {
    snprintf(out, n, "%uh%um", (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
  }
}

static lv_obj_t *makeArc(lv_obj_t *parent) {
  lv_obj_t *a = lv_arc_create(parent);
  lv_obj_set_size(a, ARC_SIZE, ARC_SIZE);
  lv_obj_center(a);
  lv_arc_set_rotation(a, 0);
  lv_arc_set_bg_angles(a, 0, 0);
  // The arc widget is an input control by default. Here it is pure output -
  // touches are resolved by angle at the screen level instead, because arc
  // bounding boxes all overlap at the centre and would fight over the press.
  lv_obj_remove_style(a, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, ARC_W, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(a, false, LV_PART_INDICATOR);
  lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
  return a;
}

#if STARGATE
// Ring 0 is the core disc and the last is the rim, so the ramp runs from a
// hot cyan-white centre out to deep blue - the way the real pool is lit.
//
// Kept as plain components rather than reading them back off an lv_color_t:
// with LV_COLOR_16_SWAP the 16-bit layout splits green across two bitfields,
// so `.ch.green` does not even exist, and a read-back would lose precision to
// 5/6-bit quantisation on the way through regardless.
static void horizonRGB(size_t ring, uint8_t &r, uint8_t &g, uint8_t &b) {
  float t = GATE_RINGS > 1 ? (float)ring / (float)(GATE_RINGS - 1) : 0.0f;
  r = (uint8_t)(150 - t * 146);
  g = (uint8_t)(240 - t * 205);
  b = (uint8_t)(255 - t * 140);
}

static lv_color_t horizonColour(size_t ring) {
  uint8_t r, g, b;
  horizonRGB(ring, r, g, b);
  return lv_color_make(r, g, b);
}

static void buildGate(lv_obj_t *scr) {
  // Filled circles, not arcs. Two reasons, and they agree for once.
  //
  // Visually: the event horizon is a luminous pool, not a set of outlines.
  // Stacking discs from rim to core gives the radial ramp the real thing has.
  //
  // Cost: an lv_arc is drawn through an arc mask over its entire bounding box,
  // and twelve full-circle arcs measured 160 ms a frame - six frames a second.
  // A filled circle is a rounded-rect fill, which is far cheaper per pixel.
  //
  // Created smallest first, each pushed to the back, so the largest ends up
  // furthest back and every disc above it stays visible.
  for (size_t i = 0; i < GATE_RINGS; i++) {
    lv_coord_t d = GATE_INNER + (lv_coord_t)i * GATE_STEP;
    lv_obj_t *c = lv_obj_create(scr);
    lv_obj_set_size(c, d, d);
    lv_obj_center(c);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_bg_color(c, horizonColour(i), 0);
    lv_obj_set_style_bg_opa(c, GATE_MIN, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_background(c);
    gateRipple[i] = c;
  }

  for (size_t k = 0; k < CHEVRONS; k++) {
    lv_obj_t *c = makeArc(scr);
    lv_obj_set_size(c, CHEVRON_SIZE, CHEVRON_SIZE);
    lv_obj_center(c);
    // Nine markers at 40 degree intervals, starting at 12 o'clock.
    uint16_t centreDeg = (uint16_t)((270 + k * 40) % 360);
    lv_arc_set_angles(c, (centreDeg + 356) % 360, (centreDeg + 4) % 360);
    lv_obj_set_style_arc_width(c, 9, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(c, true, LV_PART_INDICATOR);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_background(c);
    chevron[k] = c;
  }
}

void uiKawoosh() {
  g_burstStart = millis();
  g_bursting = true;
}

// Where the leading edge of the vortex sits, 0 at the centre and 1 at the rim,
// as a function of progress through the burst.
//
// Shaped after the real effect rather than a plain expansion: it erupts fast
// and decelerating, flushes outward past the rim, then snaps back and collapses
// into the plane of the gate, which is where the horizon takes over.
static float vortexFront(float t) {
  if (t < 0.34f) return powf(t / 0.34f, 0.55f) * 1.06f;  // erupt
  if (t < 0.58f) return 1.06f;                            // flush
  if (t < 0.80f) return 1.06f - (t - 0.58f) / 0.22f * 0.5f;  // retract
  return 0.56f - (t - 0.80f) / 0.20f * 0.56f;             // collapse
}

// One frame of the gate. `n` locked chevrons, ripples travelling outward.
static void drawGate(size_t n, SessionState top, uint32_t phase) {
  bool open = n > 0;

  uint32_t elapsed = millis() - g_burstStart;
  if (g_bursting && elapsed >= KAWOOSH_MS) g_bursting = false;

  if (g_bursting) {
    float t = (float)elapsed / (float)KAWOOSH_MS;
    float front = vortexFront(t);
    // Fade the whole thing out over the collapse so it hands over to the
    // settled horizon rather than simply vanishing.
    float envelope = t < 0.80f ? 1.0f : 1.0f - (t - 0.80f) / 0.20f;

    for (size_t i = 0; i < GATE_RINGS; i++) {
      float r = (float)i / (float)(GATE_RINGS - 1);
      float d = r - front;

      float level;
      if (d > 0.0f) {
        // Ahead of the front: nothing, with a sharp leading edge.
        level = d < 0.10f ? (1.0f - d / 0.10f) : 0.0f;
      } else {
        // Behind it: the trailing column, fading toward the centre.
        level = 0.85f + d * 0.55f;
        if (level < 0.0f) level = 0.0f;
      }
      level *= envelope;

      // Scaled against GATE_MAX rather than full opacity: eight discs stack,
      // so per-layer values this side of opaque still composite to a bright
      // core - and an opaque core would swallow the text sitting on it.
      uint16_t opa = (uint16_t)(level * 200.0f);
      if (opa > 200) opa = 200;
      lv_obj_set_style_bg_opa(gateRipple[i], (lv_opa_t)opa, 0);

      // The leading edge runs hot toward white; behind it the pool stays blue.
      float heat = 1.0f - fabsf(d) / 0.28f;
      if (heat < 0.0f) heat = 0.0f;
      uint8_t br, bg, bb;
      horizonRGB(i, br, bg, bb);
      lv_color_t hot = lv_color_make((uint8_t)(br + heat * (235 - br)),
                                     (uint8_t)(bg + heat * (250 - bg)), 255);
      lv_obj_set_style_bg_color(gateRipple[i], hot, 0);
    }
  } else {
    for (size_t i = 0; i < GATE_RINGS; i++) {
      // Outer discs lag the inner ones, so the shimmer travels outward rather
      // than the whole pool breathing at once.
      uint8_t s = motionSine((uint16_t)((phase / 3 + i * 6) % 64));
      uint16_t opa = GATE_MIN + (uint16_t)s * (GATE_MAX - GATE_MIN) / 255;
      // Dormant when nothing is running: an idle gate has no puddle.
      if (!open) opa = GATE_MIN / 2 + opa / 6;
      lv_obj_set_style_bg_opa(gateRipple[i], (lv_opa_t)opa, 0);
      lv_obj_set_style_bg_color(gateRipple[i], horizonColour(i), 0);
    }
  }

  // Chevrons only change when the session count or the urgency does, but they
  // are arcs sized to the full band - restyling all nine every frame meant
  // nine full-size arc masks per frame for a picture that had not changed.
  static size_t lastN = SIZE_MAX;
  static SessionState lastTop = ST_NONE;
  if (n != lastN || top != lastTop) {
    lastN = n;
    lastTop = top;
    (void)top;
    for (size_t k = 0; k < CHEVRONS; k++) {
      bool locked = k < n;
      lv_obj_set_style_arc_color(
          chevron[k], lv_color_hex(locked ? CHEVRON_LIT : CHEVRON_DARK),
          LV_PART_INDICATOR);
      lv_obj_set_style_arc_opa(chevron[k], locked ? LV_OPA_COVER : 120,
                               LV_PART_INDICATOR);
    }
  }
}
#else   // STARGATE
void uiKawoosh() {}  // keeps /kawoosh linking when the gate is compiled out
#endif  // STARGATE

static void hideDetail() {
  g_detailOpen = false;
  g_detailSid[0] = '\0';
  lv_obj_add_flag(detail, LV_OBJ_FLAG_HIDDEN);
}

static void showDetail(const Session &s) {
  strncpy(g_detailSid, s.sid, SID_LEN - 1);
  g_detailSid[SID_LEN - 1] = '\0';
  g_detailOpen = true;
  g_detailUntil = millis() + DETAIL_HOLD_MS;
  lv_obj_clear_flag(detail, LV_OBJ_FLAG_HIDDEN);
}

static void onPress(lv_event_t *e) {
  (void)e;
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);

  // Any touch while the detail card is up dismisses it. One consistent
  // gesture beats hunting for a close target on a round screen.
  if (g_detailOpen) {
    hideDetail();
    return;
  }

  float dx = (float)p.x - CX;
  float dy = (float)p.y - CY;
  float r = sqrtf(dx * dx + dy * dy);
  if (r < BAND_INNER || r > BAND_OUTER) return;

  float ang = atan2f(dy, dx) * 180.0f / (float)M_PI;
  if (ang < 0) ang += 360.0f;

  float rel = ang - (float)RING_ZERO_DEG;
  if (!RING_CLOCKWISE) rel = -rel;  // same mirroring the renderer applies
  while (rel < 0) rel += 360.0f;
  while (rel >= 360.0f) rel -= 360.0f;
  float led = rel * (float)g_leds / 360.0f;

  for (size_t i = 0; i < g_spanN && i < g_shownN; i++) {
    if (led >= g_spans[i].lo && led < g_spans[i].hi) {
      showDetail(g_shown[i]);
      return;
    }
  }
}

void uiInit(const Palette &palette, uint16_t ledCount) {
  g_pal = &palette;
  g_leds = ledCount ? ledCount : 24;

  lv_obj_t *scr = lv_scr_act();
  // Black is free on an AMOLED - unlit pixels draw nothing - so the arcs sit
  // on true black rather than a dark grey panel.
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(scr, onPress, LV_EVENT_PRESSED, NULL);

  for (size_t i = 0; i < MAX_SESSIONS; i++) {
    arcBase[i] = makeArc(scr);
    arcHead[i] = makeArc(scr);
  }

#if STARGATE
  buildGate(scr);
#endif

  lblCount = lv_label_create(scr);
  lv_obj_set_style_text_font(lblCount, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(lblCount, lv_color_white(), 0);
  lv_obj_align(lblCount, LV_ALIGN_CENTER, 0, -34);
  lv_label_set_text(lblCount, "0");

  lblWord = lv_label_create(scr);
  lv_obj_set_style_text_font(lblWord, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lblWord, lv_color_hex(0x9aa0a6), 0);
  lv_obj_align(lblWord, LV_ALIGN_CENTER, 0, 6);
  lv_label_set_text(lblWord, "AGENTS");

  lblBreak = lv_label_create(scr);
  lv_obj_set_style_text_font(lblBreak, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lblBreak, lv_color_white(), 0);
  lv_obj_set_style_text_align(lblBreak, LV_TEXT_ALIGN_CENTER, 0);
  // Bounded and wrapping: with four states present the breakdown runs past
  // 400 px on one line, and on a round panel the ends disappear under the
  // bezel rather than merely looking cramped.
  lv_obj_set_width(lblBreak, 300);
  lv_label_set_long_mode(lblBreak, LV_LABEL_LONG_WRAP);
  lv_obj_align(lblBreak, LV_ALIGN_CENTER, 0, 46);
  lv_label_set_text(lblBreak, "");

  lblNet = lv_label_create(scr);
  lv_obj_set_style_text_font(lblNet, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblNet, lv_color_hex(0x5f6368), 0);
  lv_obj_align(lblNet, LV_ALIGN_CENTER, 0, 110);
  lv_label_set_text(lblNet, "");

  // --- detail card ---------------------------------------------------------
  // Sized to the largest rectangle that stays clear of a 466 px circle's edge
  // with margin - the corners are the first thing a round panel clips.
  detail = lv_obj_create(scr);
  lv_obj_set_size(detail, 340, 250);
  lv_obj_center(detail);
  lv_obj_set_style_radius(detail, 24, 0);
  lv_obj_set_style_bg_color(detail, lv_color_hex(0x0d0d10), 0);
  lv_obj_set_style_bg_opa(detail, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(detail, 2, 0);
  lv_obj_set_style_border_color(detail, lv_color_hex(0x303036), 0);
  lv_obj_clear_flag(detail, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(detail, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(detail, LV_OBJ_FLAG_HIDDEN);

  // Name: the label if one was set, else the directory.
  lblDetailName = lv_label_create(detail);
  lv_obj_set_style_text_font(lblDetailName, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(lblDetailName, lv_color_white(), 0);
  lv_label_set_long_mode(lblDetailName, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lblDetailName, 300);
  lv_obj_set_style_text_align(lblDetailName, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lblDetailName, LV_ALIGN_TOP_MID, 0, 4);

  // Path, dimmed: what separates two sessions with the same directory name.
  lblDetailPath = lv_label_create(detail);
  lv_obj_set_style_text_font(lblDetailPath, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblDetailPath, lv_color_hex(0x6b7075), 0);
  lv_label_set_long_mode(lblDetailPath, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lblDetailPath, 300);
  lv_obj_set_style_text_align(lblDetailPath, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lblDetailPath, LV_ALIGN_TOP_MID, 0, 40);

  // The topic - the session's first prompt, condensed. This is the line that
  // separates three sessions sharing one directory, so it gets the most room
  // and it never changes for the life of the session.
  lblDetailTopic = lv_label_create(detail);
  lv_obj_set_style_text_font(lblDetailTopic, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblDetailTopic, lv_color_hex(0xe8eaed), 0);
  lv_label_set_long_mode(lblDetailTopic, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lblDetailTopic, 296);
  lv_obj_set_height(lblDetailTopic, 54);
  lv_obj_set_style_text_align(lblDetailTopic, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lblDetailTopic, LV_ALIGN_CENTER, 0, -12);

  // What it moved on to, shown only while the session is actually running and
  // only when it differs from the topic - otherwise it is noise.
  lblDetailNow = lv_label_create(detail);
  lv_obj_set_style_text_font(lblDetailNow, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblDetailNow, lv_color_hex(0x8a8f94), 0);
  lv_label_set_long_mode(lblDetailNow, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lblDetailNow, 296);
  lv_obj_set_style_text_align(lblDetailNow, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lblDetailNow, LV_ALIGN_CENTER, 0, 30);

  lblDetailState = lv_label_create(detail);
  lv_obj_set_style_text_font(lblDetailState, &lv_font_montserrat_20, 0);
  lv_obj_align(lblDetailState, LV_ALIGN_BOTTOM_MID, 0, -34);

  lblDetailMeta = lv_label_create(detail);
  lv_obj_set_style_text_font(lblDetailMeta, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblDetailMeta, lv_color_hex(0x9aa0a6), 0);
  lv_obj_set_style_text_align(lblDetailMeta, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lblDetailMeta, LV_ALIGN_BOTTOM_MID, 0, -8);
}

void uiSetNetwork(const char *text) {
  if (lblNet) lv_label_set_text(lblNet, text ? text : "");
}

static void drawArcs(const Session *sessions, size_t n, uint32_t phase,
                     uint32_t lingerMs) {
  g_spanN = computeArcs(n, g_leds, g_spans, MAX_SESSIONS);

  for (size_t i = 0; i < MAX_SESSIONS; i++) {
    if (i >= g_spanN) {
      lv_obj_add_flag(arcBase[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(arcHead[i], LV_OBJ_FLAG_HIDDEN);
      baseCache[i].init = false;
      headCache[i].init = false;
      continue;
    }

    const Session &s = sessions[i];
    SessionState st = displayState(s);
    uint16_t lo = g_spans[i].lo, hi = g_spans[i].hi;
    uint16_t span = hi - lo;
    lv_color_t col = colourOf(st);

    lv_obj_clear_flag(arcBase[i], LV_OBJ_FLAG_HIDDEN);
    if (span >= g_leds) {
      // A lone session owns the whole ring, and there ledToDeg(0) and
      // ledToDeg(ledCount) are the same angle - which would collapse the arc
      // to nothing instead of drawing a full circle.
      if (!baseCache[i].init || baseCache[i].start != 0 ||
          baseCache[i].end != 360) {
        lv_arc_set_angles(arcBase[i], 0, 360);
        baseCache[i].start = 0;
        baseCache[i].end = 360;
      }
    } else {
      setArcSpan(arcBase[i], baseCache[i], lo, hi);
    }

    // Same stagger the ring uses, so neighbours are never in step.
    uint16_t phaseOff = (uint16_t)(i * 64 / g_spanN);
    uint16_t cometOff = (uint16_t)(i * span / g_spanN);

    if (st == ST_DONE) {
      uint32_t elapsed = s.hasEnded ? (millis() - s.ended) : 0;
      uint32_t linger = lingerMs ? lingerMs : 1;
      int32_t fade = 255 - (int32_t)(elapsed * 255 / linger);
      if (fade < 0) fade = 0;
      setArcStyle(arcBase[i], baseCache[i], col, quantise((uint16_t)fade));
      lv_obj_add_flag(arcHead[i], LV_OBJ_FLAG_HIDDEN);
      headCache[i].init = false;
      continue;
    }

    MotionSpec m = motionFor(st);
    if (m.comet) {
      // The body never changes once set, so after the first frame a comet
      // costs only its head's angle move - a sector invalidation, not a
      // full-screen one. This is why comets are nearly free and pulses are not.
      setArcStyle(arcBase[i], baseCache[i], col, COMET_BODY);
      uint16_t headOff = cometHead(span, phase, cometOff, m.rate);
      // Head plus one LED of tail, so the direction of travel reads.
      float headLed = (float)(lo + headOff);
      lv_obj_clear_flag(arcHead[i], LV_OBJ_FLAG_HIDDEN);
      setArcSpan(arcHead[i], headCache[i], headLed, headLed + 1.0f);
      setArcStyle(arcHead[i], headCache[i], col, LV_OPA_COVER);
    } else {
      uint8_t level = pulseLevel(phase, m.rate, phaseOff);
      if (level < PULSE_FLOOR) level = PULSE_FLOOR;
      // Quantised: a pulse is the one animation that must change a style every
      // frame, and each change costs a full-bbox invalidation. Rounding to 16
      // levels cuts that to a few writes per breath with no visible stepping.
      setArcStyle(arcBase[i], baseCache[i], col, quantise(level));
      lv_obj_add_flag(arcHead[i], LV_OBJ_FLAG_HIDDEN);
      headCache[i].init = false;
    }
  }
}

static void drawCentre(const Session *sessions, size_t n) {
  if (n == 0) {
    // Dormant gate: no pool to read against, so the pale scheme applies.
    lv_label_set_text(lblCount, "-");
    lv_obj_set_style_text_color(lblCount, lv_color_hex(0x3c4043), 0);
    lv_obj_set_style_text_color(lblWord, lv_color_hex(0x9aa0a6), 0);
    lv_label_set_text(lblWord, "IDLE");
    lv_label_set_text(lblBreak, "");
    return;
  }

  uint8_t byState[5] = {0, 0, 0, 0, 0};
  SessionState top = ST_WORKING;
  for (size_t i = 0; i < n; i++) {
    SessionState st = displayState(sessions[i]);
    byState[st]++;
    if (urgency(st) > urgency(top)) top = st;
  }

  lv_label_set_text_fmt(lblCount, "%u", (unsigned)n);

  // With the event horizon lit, the centre is a bright cyan pool and pale text
  // disappears into it - so the summary is drawn as dark ink on the horizon
  // instead. The cost is that the headline number no longer carries the
  // urgency colour; that signal still has the arcs and the whole LED ring,
  // which are the louder channels anyway. With STARGATE off, or an idle gate,
  // the old colouring returns.
#if STARGATE
  bool onPool = true;
#else
  bool onPool = false;
#endif
  lv_obj_set_style_text_color(
      lblCount, onPool ? lv_color_hex(0x03151f) : colourOf(top), 0);
  lv_obj_set_style_text_color(
      lblWord, lv_color_hex(onPool ? 0x0a3245 : 0x9aa0a6), 0);
  lv_label_set_text(lblWord, n == 1 ? "AGENT" : "AGENTS");

  // Most urgent first: what is blocked matters more than what is busy.
  static const SessionState ORDER[] = {ST_ATTENTION, ST_INPUT, ST_WORKING,
                                       ST_BACKGROUND, ST_DONE};
  static const char *SHORT[] = {"working", "background", "waiting", "needs you",
                                "done"};
  char buf[64];
  buf[0] = '\0';
  for (size_t k = 0; k < 5; k++) {
    SessionState st = ORDER[k];
    if (!byState[st]) continue;
    char part[24];
    snprintf(part, sizeof(part), "%s%u %s", buf[0] ? "  " : "",
             (unsigned)byState[st], SHORT[st]);
    if (strlen(buf) + strlen(part) < sizeof(buf)) strcat(buf, part);
  }
  lv_label_set_text(lblBreak, buf);
  lv_obj_set_style_text_color(
      lblBreak, lv_color_hex(onPool ? 0x072634 : 0xc8ccd0), 0);
}

static void drawDetail() {
  if (!g_detailOpen) return;

  if ((int32_t)(millis() - g_detailUntil) >= 0) {
    hideDetail();
    return;
  }

  const Session *found = nullptr;
  for (size_t i = 0; i < g_shownN; i++) {
    if (strcmp(g_shown[i].sid, g_detailSid) == 0) {
      found = &g_shown[i];
      break;
    }
  }
  if (!found) {  // the session expired while its card was open
    hideDetail();
    return;
  }

  const Session &s = *found;
  SessionState st = displayState(s);
  lv_color_t col = colourOf(st);

  lv_label_set_text(lblDetailName, s.displayName());
  lv_label_set_text(lblDetailPath, s.path[0] ? s.path : "");
  // Identity line. The topic is stable and is what tells two sessions in one
  // folder apart, so it wins. A session first seen mid-flight has no first
  // prompt to remember, and then whatever it last said is the better identity
  // than nothing at all.
  const char *topic = s.topic[0]   ? s.topic
                      : s.prompt[0] ? s.prompt
                                    : s.reply;
  if (topic[0]) {
    // Quoted so a fragment is not mistaken for a status line.
    lv_label_set_text_fmt(lblDetailTopic, "\"%s\"", topic);
  } else {
    // Nothing yet: say so rather than leaving a hole the eye reads as a bug.
    lv_label_set_text(lblDetailTopic, "no prompt seen yet");
  }

  // Second line adapts to what the state needs answering.
  //
  //   running  -> what it moved on to, if anything
  //   waiting  -> what it just finished, which is the whole question when an
  //               arc is pulsing at you and you have not read that terminal
  bool running = (st == ST_WORKING || st == ST_BACKGROUND);
  if (st == ST_BACKGROUND && s.agents[0]) {
    // What is running underneath is the whole question for a background
    // session, and naming the agents answers it better than counting them.
    lv_label_set_text_fmt(lblDetailNow, "agents: %s", s.agents);
  } else if (running && s.prompt[0] && strcmp(s.prompt, topic) != 0) {
    lv_label_set_text_fmt(lblDetailNow, "now: %s", s.prompt);
  } else if (!running && s.reply[0] && strcmp(s.reply, topic) != 0) {
    lv_label_set_text_fmt(lblDetailNow, "said: %s", s.reply);
  } else {
    lv_label_set_text(lblDetailNow, "");
  }
  lv_label_set_text(lblDetailState, stateWord(st));
  lv_obj_set_style_text_color(lblDetailState, col, 0);
  lv_obj_set_style_border_color(detail, col, 0);

  char age[16];
  formatAge(millis() - s.updated, age, sizeof(age));
  if (s.pending > 0) {
    lv_label_set_text_fmt(lblDetailMeta, "%.8s   %s   %d %s", s.sid, age,
                          s.pending, s.pending == 1 ? "agent" : "agents");
  } else {
    lv_label_set_text_fmt(lblDetailMeta, "%.8s   %s", s.sid, age);
  }
}

void uiUpdate(const Session *sessions, size_t n, uint32_t phase,
              uint32_t lingerMs) {
  if (n > MAX_SESSIONS) n = MAX_SESSIONS;
  memcpy(g_shown, sessions, n * sizeof(Session));
  g_shownN = n;

  // Frame budget, measured on this hardware: a full-screen pass costs ~59 ms
  // without the gate and ~170 ms with it, of which only 27% is the SPI
  // transfer - the rest is LVGL compositing. Two pulsing arcs alone invalidate
  // their whole 448x448 bounding boxes, LVGL merges those into one area, and
  // every frame ends up redrawing all 217k pixels.
  //
  // Asking for 30 fps against a 59 ms frame is what produced the lurching:
  // calls arrive faster than they complete, so the visible cadence is whatever
  // is left over. Dividing the work down to a rate the panel can actually hold
  // trades frame rate for an even one, which is the thing the eye notices.
  static uint8_t tick = 0;
  tick++;

#if STARGATE
  bool bursting = g_bursting;
#else
  bool bursting = false;
#endif

  // A burst freezes the arcs for its ~1.1 s. Nothing about a comet's position
  // is worth defending against a wormhole opening, and skipping them shrinks
  // the invalidated area to the gate alone - which is what buys the surge
  // enough frames to read as a surge.
  if (!bursting && (tick % 3) == 0) {
    drawArcs(sessions, n, phase, lingerMs);
    drawCentre(sessions, n);
  }

#if STARGATE
  // A new session dials the gate. Giving the kawoosh a cause rather than a
  // timer is what stops it being a screensaver: it fires when something
  // actually happened, and the rest of the time the horizon just ripples.
  if (!g_gateSeeded) {
    g_gateSeeded = true;
    uiKawoosh();  // and once at boot, because a gate should announce itself
  } else if (n > g_prevSessions) {
    uiKawoosh();
  }
  g_prevSessions = n;

  // At rest the shimmer is slow enough to run at a sixth of the loop rate.
  // During a burst it gets every pass, and with the arcs frozen it is the only
  // thing redrawing.
  // Offset from the arc tick, not aligned to it. Both on the same tick meant
  // every sixth frame did all the work at once - measured at 138 ms, which is
  // long enough to see as a hitch. Interleaved, each heavy frame does half.
  if (bursting || (tick % 6) == 3) {
    SessionState top = ST_WORKING;
    for (size_t i = 0; i < n; i++) {
      SessionState st = displayState(sessions[i]);
      if (urgency(st) > urgency(top)) top = st;
    }
    drawGate(n, top, phase);
  }
#endif

  // The detail card rewrites five labels; no point doing that faster than the
  // arcs, and nobody reads a card at 30 fps.
  if (!bursting && (tick % 3) == 0) drawDetail();
}
