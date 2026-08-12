#include "server.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <string.h>

#include "ring.h"

static const uint32_t REQUEST_TIMEOUT_MS = 5000;

// Notification subtypes that genuinely block on a human. Red is reserved for
// these. Note that idle_prompt is NOT here: it means "this session has been
// quiet a while", which is exactly what a session waiting on background agents
// looks like. Treating it as blocking made the ring cry wolf, so it maps to
// INPUT instead - and even that is suppressed while background work is pending.
static const char *ATTENTION_NOTIFICATIONS[] = {
    "permission_prompt", "agent_needs_input", "elicitation_dialog"};

// Events that start a unit of background work, and those that end one.
static const char *WORK_START[] = {"SubagentStart", "TaskCreated"};
static const char *WORK_END[] = {"SubagentStop", "TaskCompleted"};

struct Classification {
  SessionState state;  // ST_NONE leaves the session's state alone
  int delta;
  bool reset;
  bool background;
};

static bool oneOf(const char *v, const char *const *list, size_t n) {
  if (!v) return false;
  for (size_t i = 0; i < n; i++) {
    if (strcmp(v, list[i]) == 0) return true;
  }
  return false;
}

// Map a hook payload to a state change, a work delta and/or a background ping.
//
// `background` refreshes the recency window. Both starts AND stops set it,
// because SubagentStart cannot be relied on: observed in the field firing once
// against eleven SubagentStops. A stream of stops is itself evidence that
// agents are running, so any background event counts.
static Classification classify(const char *event, const char *kind) {
  Classification c = {ST_NONE, 0, false, false};
  if (!event) return c;

  if (strcmp(event, "UserPromptSubmit") == 0 ||
      strcmp(event, "SessionStart") == 0) {
    // A new prompt re-baselines the counter and the window. This is also what
    // releases a session from BACKGROUND once its agents have genuinely
    // finished.
    c.state = ST_WORKING;
    c.reset = true;
    return c;
  }
  if (strcmp(event, "Stop") == 0) {
    c.state = ST_INPUT;
    return c;
  }
  if (strcmp(event, "SessionEnd") == 0) {
    c.state = ST_DONE;
    return c;
  }
  if (oneOf(event, WORK_START, 2)) {
    c.delta = 1;
    c.background = true;
    return c;
  }
  if (oneOf(event, WORK_END, 2)) {
    c.delta = -1;
    c.background = true;
    return c;
  }
  if (strcmp(event, "Notification") == 0) {
    if (oneOf(kind, ATTENTION_NOTIFICATIONS, 3)) {
      c.state = ST_ATTENTION;
      return c;
    }
    if (kind && (strcmp(kind, "idle_prompt") == 0 ||
                 strcmp(kind, "agent_completed") == 0)) {
      c.state = ST_INPUT;
      return c;
    }
    // auth_success, elicitation_complete, etc. carry no status meaning.
    return c;
  }
  return c;
}

static inline bool isSep(char c) { return c == '/' || c == '\\'; }

// The last path component of cwd, which is the project name in every layout
// that matters.
//
// Both separators are honoured because sessions arrive from Windows machines
// too: splitting on '/' alone turns C:\Users\me\Documents\Vault into
// "C:\Users\me\Documen" - the front of the path, truncated, which is the least
// useful label available.
static void basenameOf(const char *path, char *out, size_t n) {
  out[0] = '\0';
  if (!path || !path[0]) return;

  size_t len = strlen(path);
  while (len > 1 && isSep(path[len - 1])) len--;  // ignore a trailing separator

  size_t start = 0;
  for (size_t i = 0; i < len; i++) {
    if (isSep(path[i])) start = i + 1;
  }
  size_t take = len - start;
  if (take >= n) take = n - 1;
  memcpy(out, path + start, take);
  out[take] = '\0';
}

// The tail of a path, so two sessions in same-named directories are tellable
// apart. The tail rather than the head because the distinguishing part of
// ~/work/api vs ~/personal/api is nearer the end, and $HOME is collapsed to ~
// since it is the same for every session on a machine.
static void tailPath(const char *path, char *out, size_t n) {
  out[0] = '\0';
  if (!path || !path[0]) return;

  // Collapse the home prefix to ~, on either platform: /home/<user>/... and
  // C:\Users\<user>\... are the same for every session on a machine and only
  // eat the width that distinguishes one project from another.
  const char *p = path;
  const char *afterHome = nullptr;
  if (strncmp(p, "/home/", 6) == 0) {
    afterHome = strchr(p + 6, '/');
  } else if (strncasecmp(p, "C:\\Users\\", 9) == 0) {
    afterHome = strchr(p + 9, '\\');
  }
  if (afterHome) {
    size_t len = strlen(afterHome);  // includes the leading separator
    if (len + 2 <= n) {
      out[0] = '~';
      memcpy(out + 1, afterHome, len + 1);
      return;
    }
    p = afterHome + 1;
  }

  size_t len = strlen(p);
  if (len < n) {
    memcpy(out, p, len + 1);
    return;
  }
  // Too long: keep the tail, marked so it is obviously truncated.
  size_t keep = n - 4;
  out[0] = out[1] = out[2] = '.';
  memcpy(out + 3, p + len - keep, keep + 1);
}

// Openers that carry no information about the work, longest first so that
// "i would like you to" wins over any shorter prefix of itself. Stored without
// a trailing space: what follows may be a space OR a comma, and requiring a
// space is what let "Hey, can you please ..." through untouched.
//
// Words that can carry meaning - "now", "next", "then", "just" - are
// deliberately absent. Stripping those loses information more often than it
// saves space.
static const char *FILLER[] = {
    "i would like you to", "i'd like you to", "id like you to",
    "i want you to",       "i need you to",   "could you please",
    "can you please",      "we need to",      "we should",
    "could you",           "would you",       "will you",
    "can you",             "help me",         "please",
    "let's",               "lets",            "okay",
    "hey",                 "hi",              "ok",
    "so",
};

static bool startsWithI(const char *s, const char *prefix) {
  while (*prefix) {
    if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) {
      return false;
    }
    s++;
    prefix++;
  }
  return true;
}

// Condense a prompt into something that fits on a card and still says what the
// session is working on.
//
// This is deliberately not summarisation - the device cannot run a model. It
// strips filler openers, keeps the first sentence, and truncates on a word
// boundary. On real prompts that recovers most of the value: "Hey, can you
// please look at why the arc alignment is off? I think..." becomes "look at
// why the arc alignment is off".
static void condense(const char *text, char *out, size_t n) {
  out[0] = '\0';
  if (!text) return;

  const char *p = text;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

  // Strip filler openers, repeatedly and in any order, so "Hey, can you please
  // index..." sheds all three. A match only counts when the opener is followed
  // by a separator - otherwise "so" would eat the front of "sort the files".
  bool stripped = true;
  while (stripped) {
    stripped = false;
    for (size_t i = 0; i < sizeof(FILLER) / sizeof(FILLER[0]); i++) {
      if (!startsWithI(p, FILLER[i])) continue;
      const char *after = p + strlen(FILLER[i]);
      if (*after != ' ' && *after != ',' && *after != ':') continue;
      p = after;
      while (*p == ' ' || *p == ',' || *p == ':') p++;
      stripped = true;
      break;
    }
  }
  // A stripped-to-nothing prompt ("please?") is worse than the original.
  if (!*p) p = text;

  // First sentence or line, whichever ends first.
  size_t i = 0;
  bool truncated = false;
  while (p[i] && p[i] != '\n' && p[i] != '\r' && p[i] != '.' && p[i] != '?' &&
         p[i] != '!') {
    if (i >= n - 1) {
      truncated = true;
      break;
    }
    out[i] = p[i];
    i++;
  }
  out[i] = '\0';

  if (truncated) {
    // Back up to a word boundary so the tail is not a fragment of a word.
    size_t cut = i;
    while (cut > 0 && out[cut - 1] != ' ') cut--;
    if (cut > n / 3) i = cut;  // only if it does not cost most of the line
    while (i > 0 && out[i - 1] == ' ') i--;
    // "..." rather than the ellipsis character: the montserrat subsets built
    // into this firmware do not carry U+2026.
    if (i + 4 <= n) {
      memcpy(out + i, "...", 4);
      return;
    }
  }

  while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == ',')) i--;
  out[i] = '\0';
}

// Percent-decoding, in place semantics: "ring%20firmware" -> "ring firmware".
static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Pulls one parameter out of a query string. Used for ?label=, which is how a
// project-level hook names its sessions without a wrapper script.
static void queryParam(const char *query, const char *key, char *out,
                       size_t n) {
  out[0] = '\0';
  if (!query || !query[0]) return;

  size_t keyLen = strlen(key);
  const char *p = query;
  while (p && *p) {
    if (strncmp(p, key, keyLen) == 0 && p[keyLen] == '=') {
      const char *v = p + keyLen + 1;
      size_t i = 0;
      while (*v && *v != '&' && i < n - 1) {
        if (*v == '%' && hexVal(v[1]) >= 0 && hexVal(v[2]) >= 0) {
          out[i++] = (char)(hexVal(v[1]) * 16 + hexVal(v[2]));
          v += 3;
        } else if (*v == '+') {
          out[i++] = ' ';
          v++;
        } else {
          out[i++] = *v++;
        }
      }
      out[i] = '\0';
      return;
    }
    p = strchr(p, '&');
    if (p) p++;
  }
}

// Feeds ArduinoJson straight from the socket, bounded by Content-Length and a
// deadline so a stalled client cannot hang the loop.
struct BodyReader {
  WiFiClient *client;
  size_t remaining;
  uint32_t deadline;

  size_t readBytes(char *buf, size_t len) {
    if (len > remaining) len = remaining;
    size_t got = 0;
    while (got < len) {
      int n = client->read((uint8_t *)buf + got, len - got);
      if (n > 0) {
        got += (size_t)n;
        remaining -= (size_t)n;
        continue;
      }
      if ((int32_t)(millis() - deadline) >= 0) break;
      if (!client->connected() && client->available() == 0) break;
      delay(1);
    }
    return got;
  }

  int read() {
    char c;
    return readBytes(&c, 1) == 1 ? (uint8_t)c : -1;
  }
};

// Reads one CRLF-terminated line. Returns the length, or -1 on timeout or a
// closed connection.
static int readLine(WiFiClient &client, char *buf, size_t n, uint32_t deadline) {
  size_t i = 0;
  while (true) {
    int c = client.read();
    if (c < 0) {
      if ((int32_t)(millis() - deadline) >= 0) return -1;
      if (!client.connected() && client.available() == 0) return -1;
      delay(1);
      continue;
    }
    if (c == '\n') break;
    if (c == '\r') continue;
    if (i < n - 1) buf[i++] = (char)c;
  }
  buf[i] = '\0';
  return (int)i;
}

HookServer::HookServer(Registry &registry, uint16_t port, const char *token)
    : _registry(registry), _server(port), _token(token), _eventCount(0),
      _wrapped(false) {}

void HookServer::begin() {
  _server.begin();
  _server.setNoDelay(true);
}

void HookServer::poll() {
  WiFiClient client = _server.accept();
  if (!client) return;
  service(client);
  client.stop();
}

void HookServer::service(WiFiClient &client) {
  uint32_t deadline = millis() + REQUEST_TIMEOUT_MS;
  char line[256];

  if (readLine(client, line, sizeof(line), deadline) <= 0) return;

  char method[8] = {0};
  char path[128] = {0};
  if (sscanf(line, "%7s %127s", method, path) < 2) {
    respond(client, 400);
    return;
  }

  size_t contentLength = 0;
  char token[64] = {0};
  while (true) {
    int len = readLine(client, line, sizeof(line), deadline);
    if (len < 0) return;
    if (len == 0) break;  // blank line: end of headers

    if (strncasecmp(line, "content-length:", 15) == 0) {
      contentLength = (size_t)strtoul(line + 15, nullptr, 10);
    } else if (strncasecmp(line, "x-ring-token:", 13) == 0) {
      const char *v = line + 13;
      while (*v == ' ') v++;
      strncpy(token, v, sizeof(token) - 1);
    }
  }

  if (_token && strcmp(token, _token) != 0) {
    respond(client, 401);
    return;
  }

  bool isPost = strcmp(method, "POST") == 0;

  // Split the query off the path, so routing compares against "/hook" whether
  // or not the caller appended ?label=...
  char *query = strchr(path, '?');
  if (query) *query++ = '\0';

  if (isPost && strcmp(path, "/hook") == 0) {
    routeHook(client, contentLength, query);
  } else if (strncmp(path, "/health", 7) == 0) {
    respond(client, 200, "ok");
  } else if (strncmp(path, "/events", 7) == 0) {
    sendEvents(client);
  } else if (isPost && strncmp(path, "/ringscan", 9) == 0) {
    g_ringOverride = RING_OVERRIDE_SCAN;
    g_ringTestUntil = millis() + 180000;
    respond(client, 200, "pin scan: 180s (16 red, 17 green, 18 blue)");
  } else if (isPost && strncmp(path, "/ringzero", 9) == 0) {
    // Long window: this one is read by eye and then reported back, which takes
    // longer than confirming that something lit up.
    g_ringOverride = RING_OVERRIDE_ZERO;
    g_ringTestUntil = millis() + 180000;
    respond(client, 200, "zero marker: 180s");
  } else if (isPost && strncmp(path, "/ringtest", 9) == 0) {
    g_ringOverride = RING_OVERRIDE_TEST;
    g_ringTestUntil = millis() + 15000;
    respond(client, 200, "ring test: 15s");
  } else if (isPost && strncmp(path, "/clear", 6) == 0) {
    _registry.clear();
    _eventCount = 0;
    _wrapped = false;
    respond(client, 200, "cleared");
  } else if (strcmp(path, "/") == 0 || strncmp(path, "/status", 7) == 0) {
    sendStatus(client);
  } else {
    respond(client, 404);
  }
}

void HookServer::routeHook(WiFiClient &client, size_t contentLength,
                           const char *query) {
  // The filter is the privacy boundary, and it is enforced during parsing:
  // fields not named here are skipped as they stream past and are never
  // allocated. last_assistant_message, message, tool_input and
  // transcript_path all fall on the far side of it.
  //
  // user_prompt is deliberately on this side: its first line is by far the
  // best label a session can have, and it already crosses the LAN in every
  // payload - admitting it changes what is shown on a desk, not what travels.
  JsonDocument filter;
  filter["session_id"] = true;
  filter["hook_event_name"] = true;
  filter["notification_type"] = true;
  filter["stop_reason"] = true;
  filter["cwd"] = true;
  filter["user_prompt"] = true;

  BodyReader reader{&client, contentLength, millis() + REQUEST_TIMEOUT_MS};
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, reader, DeserializationOption::Filter(filter));

  // Swallow whatever is left of the body. Closing a socket with unread data
  // waiting sends a TCP reset, and the client can see that instead of our 200.
  while (reader.remaining) {
    char sink[128];
    if (reader.readBytes(sink, sizeof(sink)) == 0) break;
  }

  if (err) {
    Serial.printf("hook: bad payload: %s\n", err.c_str());
    // Still a 200: a malformed body is our problem, not the session's, and a
    // non-2xx would log an error in every Claude Code turn.
    respond(client, 200);
    return;
  }

  const char *sid = doc["session_id"] | "";
  const char *event = doc["hook_event_name"] | (const char *)nullptr;
  const char *kind = doc["notification_type"] | (const char *)nullptr;
  if (!kind) kind = doc["stop_reason"] | (const char *)nullptr;
  const char *cwd = doc["cwd"] | (const char *)nullptr;
  const char *userPrompt = doc["user_prompt"] | (const char *)nullptr;

  Classification c = classify(event, kind);

  if (sid[0] && (c.state != ST_NONE || c.delta || c.reset || c.background)) {
    char project[PROJECT_LEN];
    basenameOf(cwd, project, sizeof(project));

    char shortPath[PATH_LEN];
    tailPath(cwd, shortPath, sizeof(shortPath));

    char firstLine[PROMPT_LEN];
    condense(userPrompt, firstLine, sizeof(firstLine));

    char label[LABEL_LEN];
    queryParam(query, "label", label, sizeof(label));

    SessionUpdate u;
    u.state = c.state;
    u.delta = c.delta;
    u.resetPending = c.reset;
    u.background = c.background;
    u.project = project[0] ? project : nullptr;
    u.path = shortPath[0] ? shortPath : nullptr;
    u.prompt = firstLine[0] ? firstLine : nullptr;
    u.label = label[0] ? label : nullptr;

    _registry.apply(sid, u);

    // Read back what the session now looks like, for the log and the console.
    static Session snap[MAX_SESSIONS];  // static: ~4.3 KB is too much stack
    size_t n = _registry.snapshot(snap, MAX_SESSIONS);
    SessionState shown = ST_NONE;
    int16_t pending = 0;
    for (size_t i = 0; i < n; i++) {
      if (strcmp(snap[i].sid, sid) == 0) {
        shown = displayState(snap[i]);
        pending = snap[i].pending;
        break;
      }
    }

    record(sid, event, kind, shown, pending);
    Serial.printf("hook: %s %s %.8s -> %s pending=%d\n", event ? event : "?",
                  kind ? kind : "", sid, stateName(shown), pending);
  }

  respond(client, 200);
}

void HookServer::record(const char *sid, const char *event, const char *kind,
                        SessionState state, int16_t pending) {
  EventRecord &e = _events[_eventCount % EVENTS_MAX];
  e.t = millis();
  snprintf(e.session, sizeof(e.session), "%.8s", sid ? sid : "");
  snprintf(e.event, sizeof(e.event), "%s", event ? event : "");
  snprintf(e.kind, sizeof(e.kind), "%s", kind ? kind : "");
  snprintf(e.state, sizeof(e.state), "%s", stateName(state));
  e.pending = pending;

  _eventCount++;
  if (_eventCount >= EVENTS_MAX) _wrapped = true;
}

void HookServer::sendStatus(WiFiClient &client) {
  static Session snap[MAX_SESSIONS];  // static: ~4.3 KB is too much stack
  size_t n = _registry.snapshot(snap, MAX_SESSIONS);

  JsonDocument doc;
  JsonArray arr = doc["sessions"].to<JsonArray>();
  for (size_t i = 0; i < n; i++) {
    const Session &s = snap[i];
    uint32_t age = millis() - s.updated;
    uint32_t ttl = _registry.ttlFor(s);
    JsonObject o = arr.add<JsonObject>();
    char sid8[9];
    snprintf(sid8, sizeof(sid8), "%.8s", s.sid);
    o["session"] = sid8;
    o["name"] = s.displayName();
    o["project"] = s.project;
    o["path"] = s.path;
    o["label"] = s.label;
    o["topic"] = s.topic;
    o["prompt"] = s.prompt;
    o["state"] = stateName(displayState(s));
    o["raw_state"] = stateName(s.state);
    o["pending"] = s.pending;
    o["background"] = s.busyInBackground();
    o["age_ms"] = age;
    // Makes it obvious from /status why an arc is about to vanish.
    o["expires_in_ms"] = age > ttl ? 0 : ttl - age;
  }
  doc["count"] = n;
  doc["uptime_ms"] = millis();
  doc["free_heap"] = ESP.getFreeHeap();

  String out;
  serializeJson(doc, out);
  respond(client, 200, out.c_str(), "application/json");
}

void HookServer::sendEvents(WiFiClient &client) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  size_t n = _wrapped ? EVENTS_MAX : _eventCount;
  size_t start = _wrapped ? _eventCount % EVENTS_MAX : 0;
  for (size_t i = 0; i < n; i++) {
    const EventRecord &e = _events[(start + i) % EVENTS_MAX];
    JsonObject o = arr.add<JsonObject>();
    o["t"] = e.t;
    o["session"] = e.session;
    o["event"] = e.event;
    o["kind"] = e.kind;
    o["state"] = e.state;
    o["pending"] = e.pending;
  }

  String out;
  serializeJson(doc, out);
  respond(client, 200, out.c_str(), "application/json");
}

void HookServer::respond(WiFiClient &client, int status, const char *body,
                         const char *ctype) {
  const char *reason = "OK";
  switch (status) {
    case 400: reason = "Bad Request"; break;
    case 401: reason = "Unauthorized"; break;
    case 404: reason = "Not Found"; break;
    default: break;
  }
  size_t len = strlen(body);
  client.printf(
      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
      "Connection: close\r\n\r\n",
      status, reason, ctype, (unsigned)len);
  if (len) client.write((const uint8_t *)body, len);
  client.flush();
}
