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
static lv_obj_t *lblDetailPrompt;
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

// LVGL always fills an arc clockwise from start to end, so an anticlockwise
// LED chain needs its endpoints swapped or every arc is drawn as its
// complement - the gap instead of the segment.
static void setArcSpan(lv_obj_t *arc, float loLed, float hiLed) {
  uint16_t a = ledToDeg(loLed), b = ledToDeg(hiLed);
  if (RING_CLOCKWISE) {
    lv_arc_set_angles(arc, a, b);
  } else {
    lv_arc_set_angles(arc, b, a);
  }
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
  lv_obj_align(lblBreak, LV_ALIGN_CENTER, 0, 44);
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

  // The prompt, wrapped over two lines. This is the line that actually answers
  // "which agent is this", so it gets the most room.
  lblDetailPrompt = lv_label_create(detail);
  lv_obj_set_style_text_font(lblDetailPrompt, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblDetailPrompt, lv_color_hex(0xc8ccd0), 0);
  lv_label_set_long_mode(lblDetailPrompt, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lblDetailPrompt, 296);
  lv_obj_set_height(lblDetailPrompt, 40);
  lv_obj_set_style_text_align(lblDetailPrompt, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lblDetailPrompt, LV_ALIGN_CENTER, 0, 2);

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
      lv_arc_set_angles(arcBase[i], 0, 360);
    } else {
      setArcSpan(arcBase[i], lo, hi);
    }
    lv_obj_set_style_arc_color(arcBase[i], col, LV_PART_INDICATOR);

    // Same stagger the ring uses, so neighbours are never in step.
    uint16_t phaseOff = (uint16_t)(i * 64 / g_spanN);
    uint16_t cometOff = (uint16_t)(i * span / g_spanN);

    if (st == ST_DONE) {
      uint32_t elapsed = s.hasEnded ? (millis() - s.ended) : 0;
      uint32_t linger = lingerMs ? lingerMs : 1;
      int32_t fade = 255 - (int32_t)(elapsed * 255 / linger);
      if (fade < 0) fade = 0;
      lv_obj_set_style_arc_opa(arcBase[i], (lv_opa_t)fade, LV_PART_INDICATOR);
      lv_obj_add_flag(arcHead[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }

    MotionSpec m = motionFor(st);
    if (m.comet) {
      lv_obj_set_style_arc_opa(arcBase[i], COMET_BODY, LV_PART_INDICATOR);
      uint16_t headOff = cometHead(span, phase, cometOff, m.rate);
      // Head plus one LED of tail, so the direction of travel reads.
      float headLed = (float)(lo + headOff);
      lv_obj_clear_flag(arcHead[i], LV_OBJ_FLAG_HIDDEN);
      setArcSpan(arcHead[i], headLed, headLed + 1.0f);
      lv_obj_set_style_arc_color(arcHead[i], col, LV_PART_INDICATOR);
      lv_obj_set_style_arc_opa(arcHead[i], LV_OPA_COVER, LV_PART_INDICATOR);
    } else {
      uint8_t level = pulseLevel(phase, m.rate, phaseOff);
      if (level < PULSE_FLOOR) level = PULSE_FLOOR;
      lv_obj_set_style_arc_opa(arcBase[i], level, LV_PART_INDICATOR);
      lv_obj_add_flag(arcHead[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void drawCentre(const Session *sessions, size_t n) {
  if (n == 0) {
    lv_label_set_text(lblCount, "-");
    lv_obj_set_style_text_color(lblCount, lv_color_hex(0x3c4043), 0);
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
  // The headline number takes the colour of the most urgent session, so the
  // centre answers "does anything want me?" before any text is read.
  lv_obj_set_style_text_color(lblCount, colourOf(top), 0);
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
  lv_obj_set_style_text_color(lblBreak, lv_color_hex(0xc8ccd0), 0);
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
  // Quoted so a prompt fragment is not mistaken for a status line.
  if (s.prompt[0]) {
    lv_label_set_text_fmt(lblDetailPrompt, "\"%s\"", s.prompt);
  } else {
    lv_label_set_text(lblDetailPrompt, "");
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

  drawArcs(sessions, n, phase, lingerMs);
  drawCentre(sessions, n);
  drawDetail();
}
