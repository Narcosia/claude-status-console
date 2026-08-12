// HTTP endpoints for Claude Code's native `"type": "http"` hooks.
//
//   POST /hook    hook payload from Claude Code
//   GET  /        session states as JSON
//   GET  /health  bare liveness check
//   GET  /events  last 40 events (metadata only) for diagnosis
//   POST /clear   forget all sessions
//
// Responses to /hook are always 200 with an EMPTY body. A 2xx with a text body
// would be fed back into the session as added context - not something a status
// light should ever do.
//
// This is a hand-rolled server on WiFiServer rather than the WebServer library
// for one reason: WebServer buffers the whole request body into a String
// before a handler sees it, and hook payloads carry `user_input` and
// `last_assistant_message`, which can run to tens of kilobytes. Here the body
// is parsed straight off the socket through an ArduinoJson filter, so the
// fields this device does not want are discarded as they arrive and never
// occupy memory at all.

#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include "sessions.h"

struct EventRecord {
  uint32_t t;
  char session[9];  // truncated session id, as the console print does
  char event[28];
  char kind[28];
  char state[12];
  int16_t pending;
};

static const size_t EVENTS_MAX = 40;

class HookServer {
 public:
  HookServer(Registry &registry, uint16_t port, const char *token);

  void begin();

  // Accept and service at most one client. Called from the main loop, so the
  // registry is only mutated from this task and the UI task reads snapshots.
  void poll();

 private:
  void service(WiFiClient &client);
  // `query` is whatever followed '?' in the request path, so a project-level
  // hook can name its session with ?label=...
  void routeHook(WiFiClient &client, size_t contentLength, const char *query);
  void respond(WiFiClient &client, int status, const char *body = "",
               const char *ctype = "text/plain");
  void sendStatus(WiFiClient &client);
  void sendEvents(WiFiClient &client);
  void record(const char *sid, const char *event, const char *kind,
              SessionState state, int16_t pending);

  Registry &_registry;
  WiFiServer _server;
  const char *_token;

  EventRecord _events[EVENTS_MAX];
  size_t _eventCount;  // total written; index with % EVENTS_MAX once wrapped
  bool _wrapped;
};
