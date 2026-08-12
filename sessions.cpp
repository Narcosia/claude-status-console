#include "sessions.h"

#include <string.h>

// millis() wraps every ~49 days. Casting the difference to a signed value makes
// comparisons wrap correctly, the same trick MicroPython's ticks_diff uses.
// Valid for intervals under ~24 days, and the longest TTL here is hours.
static inline int32_t since(uint32_t t) {
  return (int32_t)(millis() - t);
}

const char *stateName(SessionState s) {
  switch (s) {
    case ST_WORKING: return "working";
    case ST_BACKGROUND: return "background";
    case ST_INPUT: return "input";
    case ST_ATTENTION: return "attention";
    case ST_DONE: return "done";
    default: return "none";
  }
}

uint8_t urgency(SessionState s) {
  switch (s) {
    case ST_WORKING: return 0;
    case ST_BACKGROUND: return 0;
    case ST_DONE: return 1;
    case ST_INPUT: return 2;
    case ST_ATTENTION: return 3;
    default: return 0;
  }
}

bool Session::busyInBackground() const {
  if (pending > 0) return true;
  if (!hasBg) return false;
  return (int32_t)(bgUntil - millis()) > 0;
}

SessionState displayState(const Session &s) {
  if (s.state == ST_ATTENTION || s.state == ST_DONE) return s.state;
  if (s.busyInBackground()) return ST_BACKGROUND;
  return s.state;
}

Registry::Registry(uint32_t ttlMs, uint32_t workingTtlMs, uint32_t doneLingerMs,
                   uint32_t backgroundHoldMs)
    : _count(0),
      _ttl(ttlMs),
      _workingTtl(workingTtlMs),
      _linger(doneLingerMs),
      _bgHold(backgroundHoldMs),
      _counter(0) {
  _lock = xSemaphoreCreateMutex();
}

uint32_t Registry::ttlFor(const Session &s) const {
  // Keyed on the raw state, not the displayed one: a session held at 'working'
  // only by a pending counter should still expire on the short TTL if its
  // background work never reports back.
  return s.state == ST_WORKING ? _workingTtl : _ttl;
}

Session *Registry::_find(const char *sid) {
  for (size_t i = 0; i < _count; i++) {
    if (strncmp(_sessions[i].sid, sid, SID_LEN - 1) == 0) return &_sessions[i];
  }
  return nullptr;
}

Session *Registry::_getOrCreate(const char *sid) {
  Session *s = _find(sid);
  if (s) return s;

  // An unknown session_id showing up mid-flight is normal: the device may have
  // rebooted, or hooks were added to an already-running session. Register it
  // rather than dropping the event.
  if (_count >= MAX_SESSIONS) {
    // Table full. Evict the least urgent, oldest session - the one whose
    // disappearance is least likely to be noticed. A wrongly evicted session
    // reappears on its next event, same as a wrongly pruned one.
    size_t victim = 0;
    for (size_t i = 1; i < _count; i++) {
      uint8_t vu = urgency(displayState(_sessions[victim]));
      uint8_t iu = urgency(displayState(_sessions[i]));
      if (iu < vu || (iu == vu && since(_sessions[i].updated) >
                                      since(_sessions[victim].updated))) {
        victim = i;
      }
    }
    _sessions[victim] = _sessions[_count - 1];
    _count--;
  }

  s = &_sessions[_count++];
  memset(s, 0, sizeof(*s));
  strncpy(s->sid, sid, SID_LEN - 1);
  s->sid[SID_LEN - 1] = '\0';
  s->project[0] = '\0';
  s->state = ST_WORKING;
  s->order = ++_counter;
  s->updated = millis();
  s->hasEnded = false;
  s->pending = 0;
  s->hasBg = false;
  return s;
}

// "set if present, leave alone if absent" - see SessionUpdate.
static void setIfGiven(char *dst, size_t cap, const char *src) {
  if (!src || !src[0]) return;
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

const char *Session::displayName() const {
  if (label[0]) return label;
  if (project[0]) return project;
  return sid;
}

void Registry::apply(const char *sid, const SessionUpdate &u) {
  if (!sid || !sid[0]) return;
  xSemaphoreTake(_lock, portMAX_DELAY);

  Session *s = _getOrCreate(sid);

  setIfGiven(s->project, PROJECT_LEN, u.project);
  setIfGiven(s->path, PATH_LEN, u.path);
  setIfGiven(s->prompt, PROMPT_LEN, u.prompt);
  setIfGiven(s->label, LABEL_LEN, u.label);

  int delta = u.delta;
  bool background = u.background;
  bool resetPending = u.resetPending;
  SessionState state = u.state;

  if (resetPending) {
    // A new prompt means the previous turn's background work is over. This is
    // also what stops the window holding a session in BACKGROUND after its
    // agents have finished.
    s->pending = 0;
    s->hasBg = false;
  }
  if (delta) {
    int v = s->pending + delta;
    s->pending = v < 0 ? 0 : (int16_t)v;
  }
  if (background) {
    s->bgUntil = millis() + _bgHold;
    s->hasBg = true;
  }

  if (state != ST_NONE) {
    s->state = state;
    s->updated = millis();
    if (state == ST_DONE) {
      s->ended = millis();
      s->hasEnded = true;
    } else {
      s->hasEnded = false;
    }
  } else {
    // A background ping is activity even with no state transition; without
    // this the session could age out mid-background-run.
    s->updated = millis();
  }

  xSemaphoreGive(_lock);
}

void Registry::prune() {
  xSemaphoreTake(_lock, portMAX_DELAY);
  for (size_t i = 0; i < _count;) {
    Session &s = _sessions[i];
    bool drop;
    if (s.state == ST_DONE && s.hasEnded) {
      drop = since(s.ended) > (int32_t)_linger;
    } else {
      drop = since(s.updated) > (int32_t)ttlFor(s);
    }
    if (drop) {
      _sessions[i] = _sessions[_count - 1];
      _count--;
    } else {
      i++;
    }
  }
  xSemaphoreGive(_lock);
}

void Registry::clear() {
  xSemaphoreTake(_lock, portMAX_DELAY);
  _count = 0;
  xSemaphoreGive(_lock);
}

size_t Registry::count() {
  xSemaphoreTake(_lock, portMAX_DELAY);
  size_t n = _count;
  xSemaphoreGive(_lock);
  return n;
}

size_t Registry::snapshot(Session *out, size_t max) {
  xSemaphoreTake(_lock, portMAX_DELAY);
  size_t n = _count < max ? _count : max;
  memcpy(out, _sessions, n * sizeof(Session));
  xSemaphoreGive(_lock);

  // Stable display order: insertion sort by first-seen. n is at most 16, and
  // this runs outside the lock.
  for (size_t i = 1; i < n; i++) {
    Session key = out[i];
    size_t j = i;
    while (j > 0 && out[j - 1].order > key.order) {
      out[j] = out[j - 1];
      j--;
    }
    out[j] = key;
  }
  return n;
}
