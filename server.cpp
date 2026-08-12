#include "server.h"

#include <ArduinoJson.h>
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

// The last path component of cwd, which is the project name in every layout
// that matters. This is the only payload field beyond session_id and the event
// name that this device retains.
static void basenameOf(const char *path, char *out, size_t n) {
  out[0] = '\0';
  if (!path || !path[0]) return;

  size_t len = strlen(path);
  while (len > 1 && path[len - 1] == '/') len--;  // ignore a trailing slash

  size_t start = 0;
  for (size_t i = 0; i < len; i++) {
    if (path[i] == '/') start = i + 1;
  }
  size_t take = len - start;
  if (take >= n) take = n - 1;
  memcpy(out, path + start, take);
  out[take] = '\0';
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

  if (isPost && strncmp(path, "/hook", 5) == 0) {
    routeHook(client, contentLength);
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

void HookServer::routeHook(WiFiClient &client, size_t contentLength) {
  // The filter is the privacy boundary, and it is enforced during parsing:
  // fields not named here are skipped as they stream past and are never
  // allocated. user_input, last_assistant_message, message and
  // transcript_path all fall on the far side of it.
  JsonDocument filter;
  filter["session_id"] = true;
  filter["hook_event_name"] = true;
  filter["notification_type"] = true;
  filter["stop_reason"] = true;
  filter["cwd"] = true;

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

  Classification c = classify(event, kind);

  if (sid[0] && (c.state != ST_NONE || c.delta || c.reset || c.background)) {
    char project[PROJECT_LEN];
    basenameOf(cwd, project, sizeof(project));

    _registry.apply(sid, c.state, c.delta, c.reset, c.background, project);

    // Read back what the session now looks like, for the log and the console.
    Session snap[MAX_SESSIONS];
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
  Session snap[MAX_SESSIONS];
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
    o["project"] = s.project;
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
