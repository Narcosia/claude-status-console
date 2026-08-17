// Device configuration. Copy to config.h and fill in - config.h is gitignored.
//
//   cp config.example.h config.h
//   $EDITOR config.h

#pragma once

// --- WiFi -------------------------------------------------------------------
#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"

// A static IP is worth setting: the hook URL in settings.json is a fixed
// string, so a DHCP lease change would silently break the device until you
// noticed it had stopped lighting up.
//
// Comment out USE_STATIC_IP to fall back to DHCP.
#define USE_STATIC_IP
#define STATIC_IP 192, 168, 1, 200
#define STATIC_GATEWAY 192, 168, 1, 254
#define STATIC_SUBNET 255, 255, 255, 0
#define STATIC_DNS 192, 168, 1, 254

#define HOSTNAME "claude-console"

// --- Ring -------------------------------------------------------------------
#define LED_COUNT 24  // XC4385 has 24

// Peak brightness, 0-255. 24 WS2812Bs at full white draw ~1.4 A - far past
// what USB supplies, and this board is also running an AMOLED off the same
// VBUS. 24 keeps the whole ring near ~150 mA at pulse peak. Raise it only with
// a dedicated 5 V supply sharing ground with the board.
#define RING_BRIGHTNESS 24

// --- Screen -----------------------------------------------------------------
// Panel brightness, 0-255. The AMOLED has no backlight GPIO; this is a display
// command.
#define SCREEN_BRIGHTNESS 180

// --- Server -----------------------------------------------------------------
#define HTTP_PORT 80

// Optional shared secret. If set, requests must carry `X-Ring-Token: <value>`.
// Leave as nullptr to disable. It travels in cleartext, so it guards against a
// neighbour driving your lights, not against anyone reading the payloads.
#define HOOK_TOKEN nullptr

// --- Timing -----------------------------------------------------------------
// Drop a session if nothing is heard from it for this long. Generous, because
// a session legitimately sitting awaiting your input sends no events.
#define SESSION_TTL_MS (4UL * 60UL * 60UL * 1000UL)  // 4 hours

// Shorter expiry for sessions still marked 'working'. A working session that
// has gone quiet this long has almost certainly died without firing SessionEnd
// (terminal closed, SSH dropped, crash). Pruning is safe: any later event from
// that session re-registers it immediately, so a wrong guess self-heals.
#define WORKING_TTL_MS (60UL * 60UL * 1000UL)  // 1 hour

// How long a finished session lingers before disappearing.
#define DONE_LINGER_MS 5000UL

// How long after the last background-work event a session keeps reading as
// BACKGROUND. A recency window rather than a counter: SubagentStart fires
// unreliably, so matched start/stop pairs cannot be trusted. Any background
// event refreshes it.
#define BACKGROUND_HOLD_MS (90UL * 1000UL)

// --- Theme ------------------------------------------------------------------
// "classic" (blue/green/amber/red), "neon" (cyan/green/hot pink/red), or
// "vapor" (all pink and cyan - prettier, less legible). Drives both the ring
// and the screen, so they always agree.
#define THEME "vapor"

// --- Smart lights -----------------------------------------------------------
// Tuya device on the LAN. Find the IP and device id with `tinytuya scan`; the
// local key comes from a Tuya IoT cloud project linked to your Smart Life
// account. Leave LIGHT_KEY as nullptr to build the UI without the transport.
#define LIGHT_IP  "192.168.1.50"
#define LIGHT_ID  "your-device-id"
#define LIGHT_KEY nullptr
