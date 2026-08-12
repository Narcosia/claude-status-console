// Tracks the live state of every Claude Code session reporting to this device.
//
// A C++ port of sessions.py from claude-status-ring, with one deliberate
// addition: the cwd basename is kept as `project`, because an 8-character
// session id is not enough to tell two arcs apart on a screen. Nothing else
// from the payload is stored - no prompt text, no assistant replies, no
// transcript paths. See README, "What this device knows".
//
// Shared between the HTTP task, the ring task and the UI, so every public
// method takes the registry's own mutex. Readers should call snapshot() once
// and work from the copy rather than holding the lock while they render.

#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Session states, in ascending order of "how much does this want your
// attention".
enum SessionState : uint8_t {
  ST_WORKING = 0,    // actively processing your turn
  ST_BACKGROUND,     // foreground idle, but subagents/tasks still running
  ST_INPUT,          // Stop fired: turn done, awaiting your next prompt
  ST_ATTENTION,      // permission prompt / explicitly needs input
  ST_DONE,           // SessionEnd: lingers briefly, then disappears
  ST_NONE = 0xFF,    // "leave the state alone" - not a real state
};

const char *stateName(SessionState s);

// Ranked so a single-colour fallback, or the centre summary, can pick the most
// urgent state across all sessions.
uint8_t urgency(SessionState s);

// The ring is unreadable long before this many arcs. Sessions past the limit
// are still counted in the summary, they just do not get their own arc.
static const size_t MAX_SESSIONS = 16;
static const size_t SID_LEN = 40;      // uuid plus room for a NUL
static const size_t PROJECT_LEN = 25;  // cwd basename, truncated
static const size_t PATH_LEN = 64;     // full cwd, head-truncated if longer
static const size_t PROMPT_LEN = 96;   // first line of the prompt, truncated
static const size_t LABEL_LEN = 25;    // explicit name from ?label=

// How long after the last background-work event a session keeps reading as
// BACKGROUND. A recency window rather than a counter because SubagentStart
// proved unreliable in practice - observed firing once against eleven
// SubagentStops - so matched pairs cannot be depended on. Any background
// event, start or stop, refreshes the window.
static const uint32_t DEFAULT_BACKGROUND_HOLD_MS = 90UL * 1000UL;

struct Session {
  char sid[SID_LEN];
  char project[PROJECT_LEN];  // cwd basename
  char path[PATH_LEN];        // full cwd, so same-named directories differ
  char topic[PROMPT_LEN];     // condensed FIRST prompt - stable identity
  char prompt[PROMPT_LEN];    // condensed latest prompt - current activity
  char reply[PROMPT_LEN];     // condensed last assistant message - what it did
  char label[LABEL_LEN];      // explicit name from ?label= on the hook URL
  SessionState state;
  uint32_t order;    // first-seen sequence, so arcs keep a stable position
  uint32_t updated;  // millis() of the last event
  uint32_t ended;    // millis() when DONE arrived
  bool hasEnded;
  int16_t pending;   // outstanding background work, when starts/stops pair up
  uint32_t bgUntil;  // deadline until which background work is assumed running
  bool hasBg;

  bool busyInBackground() const;

  // What to call this session on screen, most specific first. An explicit
  // label beats the directory name, which beats a hex id nobody can read.
  const char *displayName() const;
};

// One event's worth of change. A struct rather than a positional argument list
// because most fields are absent from any given event, and `apply(sid, state,
// 0, false, true, nullptr, nullptr, cwd)` is unreadable at the call site.
//
// Every text field is "set if present, leave alone if null". That is what lets
// a labelled project hook and an unlabelled global hook coexist: whichever
// carries the label wins, and the other never clears it.
struct SessionUpdate {
  SessionState state = ST_NONE;  // ST_NONE leaves the state alone
  int delta = 0;                 // background work counter delta
  bool resetPending = false;
  bool background = false;
  const char *project = nullptr;
  const char *path = nullptr;
  const char *prompt = nullptr;
  const char *reply = nullptr;
  const char *label = nullptr;
};

// The state to render, as opposed to the last event received.
//
// A session whose agents are still running has stopped emitting foreground
// events, which is indistinguishable from sitting idle - so it would show as
// 'awaiting your input' when it wants nothing from you. BACKGROUND gives that
// its own colour rather than borrowing WORKING's.
//
// ATTENTION always wins: a permission prompt during background work is real
// and does block you. DONE is terminal and never reinterpreted.
SessionState displayState(const Session &s);

class Registry {
 public:
  Registry(uint32_t ttlMs, uint32_t workingTtlMs, uint32_t doneLingerMs,
           uint32_t backgroundHoldMs = DEFAULT_BACKGROUND_HOLD_MS);

  // Record one event: a state change, a work delta, and/or a background ping.
  //
  // resetPending re-baselines both the counter and the recency window; it is
  // used on UserPromptSubmit, and is what releases a session from BACKGROUND
  // once its agents have genuinely finished and you have replied.
  void apply(const char *sid, const SessionUpdate &u);

  // Drop expired sessions and finished ones past their linger window.
  void prune();
  void clear();

  size_t count();

  // Copy the active sessions, ordered by first-seen, into `out`. Returns how
  // many were written (at most `max`). Render from this, not from the live
  // table - it means the lock is held for a copy rather than for a whole frame.
  size_t snapshot(Session *out, size_t max);

  uint32_t doneLingerMs() const { return _linger; }

  // Which TTL applies to this session, so /status can report why an arc is
  // about to vanish.
  uint32_t ttlFor(const Session &s) const;

 private:
  Session *_find(const char *sid);   // caller holds the lock
  Session *_getOrCreate(const char *sid);

  Session _sessions[MAX_SESSIONS];
  size_t _count;
  uint32_t _ttl;
  uint32_t _workingTtl;
  uint32_t _linger;
  uint32_t _bgHold;
  uint32_t _counter;
  SemaphoreHandle_t _lock;
};
