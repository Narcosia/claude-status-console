#include "lights.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <mbedtls/aes.h>
#include <math.h>
#include <string.h>
#include <time.h>

// Tuya LAN protocol 3.3.
//
// 3.3 is the easy one: every payload is AES-128-ECB under the device's local
// key, with no session negotiation. (3.4 adds an HMAC-SHA256 handshake - the
// v3.4 device on this network turned out to be something else entirely.)
//
// Frame layout, all big-endian:
//
//   55AA 0000 | seq | cmd | len | payload | crc32 | 0000 AA55
//
// where len counts the payload plus the crc and suffix, and the crc covers
// everything from the prefix through the payload.
//
// CONTROL payloads carry a 15-byte version header ("3.3" then twelve zeros)
// before the ciphertext; DP_QUERY payloads do not. That asymmetry is not
// documented anywhere obvious and is the usual reason a hand-rolled client
// gets ignored in silence.

static const uint16_t TUYA_PORT = 6668;
static const uint32_t PREFIX = 0x000055AA;
static const uint32_t SUFFIX = 0x0000AA55;
static const uint32_t CMD_CONTROL = 7;
static const uint32_t CMD_DP_QUERY = 10;

// Long enough to cross a congested 2.4 GHz network, short enough that a failed
// send cannot pile up behind a queue of button presses.
static const uint32_t NET_TIMEOUT_MS = 1200;
static const uint32_t POLL_INTERVAL_MS = 30000;

struct LightCmd {
  uint8_t dp;
  bool isBool;
  bool boolVal;
  char strVal[20];
};

static LightState g_state = {false, false, 60, 255, 180, 90};
static bool g_configured = false;
static char g_ip[16] = {0};
static char g_devId[32] = {0};
static uint8_t g_key[16] = {0};
static QueueHandle_t g_queue = nullptr;
static uint32_t g_seq = 1;

static void presetsLoad();

// --- crypto and framing -----------------------------------------------------

static uint32_t crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc >> 1) ^ (0xEDB88320 & (-(int32_t)(crc & 1)));
    }
  }
  return ~crc;
}

// AES-128-ECB with PKCS#7. Encrypts in place into `out`, returns length.
static size_t aesEncrypt(const uint8_t *in, size_t len, uint8_t *out) {
  size_t pad = 16 - (len % 16);
  size_t total = len + pad;
  uint8_t *buf = (uint8_t *)malloc(total);
  if (!buf) return 0;
  memcpy(buf, in, len);
  memset(buf + len, (uint8_t)pad, pad);

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, g_key, 128);
  for (size_t i = 0; i < total; i += 16) {
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, buf + i, out + i);
  }
  mbedtls_aes_free(&ctx);
  free(buf);
  return total;
}

static size_t aesDecrypt(const uint8_t *in, size_t len, uint8_t *out) {
  if (len == 0 || len % 16) return 0;
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_dec(&ctx, g_key, 128);
  for (size_t i = 0; i < len; i += 16) {
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, in + i, out + i);
  }
  mbedtls_aes_free(&ctx);
  uint8_t pad = out[len - 1];
  return (pad > 0 && pad <= 16 && pad <= len) ? len - pad : len;
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

// Wrap an already-encrypted payload in the Tuya frame.
static size_t frame(uint32_t cmd, const uint8_t *payload, size_t plen,
                    uint8_t *out) {
  put32(out + 0, PREFIX);
  put32(out + 4, g_seq++);
  put32(out + 8, cmd);
  put32(out + 12, plen + 8);  // payload + crc + suffix
  memcpy(out + 16, payload, plen);
  uint32_t c = crc32(out, 16 + plen);
  put32(out + 16 + plen, c);
  put32(out + 20 + plen, SUFFIX);
  return 24 + plen;
}

// --- transport --------------------------------------------------------------

static bool sendFrame(uint32_t cmd, const char *json, uint8_t *reply,
                      size_t *replyLen) {
  uint8_t enc[512];
  size_t elen = aesEncrypt((const uint8_t *)json, strlen(json), enc);
  if (!elen) return false;

  uint8_t payload[544];
  size_t plen = 0;
  if (cmd == CMD_CONTROL) {
    // The 15-byte version header, plaintext, ahead of the ciphertext.
    memcpy(payload, "3.3", 3);
    memset(payload + 3, 0, 12);
    plen = 15;
  }
  memcpy(payload + plen, enc, elen);
  plen += elen;

  uint8_t pkt[600];
  size_t pktLen = frame(cmd, payload, plen, pkt);

  WiFiClient client;
  client.setTimeout(NET_TIMEOUT_MS);
  if (!client.connect(g_ip, TUYA_PORT, NET_TIMEOUT_MS)) return false;
  client.write(pkt, pktLen);

  size_t got = 0;
  uint32_t deadline = millis() + NET_TIMEOUT_MS;
  while (got < 8 && (int32_t)(millis() - deadline) < 0) {
    int n = client.read(reply + got, 512 - got);
    if (n > 0) {
      got += n;
      deadline = millis() + 250;  // a short tail once bytes are arriving
    } else {
      delay(5);
    }
  }
  client.stop();
  *replyLen = got;
  return got > 0;
}

// Pull the dps object out of a reply, if it carries one.
static void hsvToRgb(uint16_t h, uint16_t s, uint16_t v, uint8_t &r, uint8_t &g,
                     uint8_t &b) {
  float S = s / 1000.0f, V = v / 1000.0f;
  float C = V * S;
  float X = C * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
  float m = V - C;
  float rr, gg, bb;
  if (h < 60)       { rr = C; gg = X; bb = 0; }
  else if (h < 120) { rr = X; gg = C; bb = 0; }
  else if (h < 180) { rr = 0; gg = C; bb = X; }
  else if (h < 240) { rr = 0; gg = X; bb = C; }
  else if (h < 300) { rr = X; gg = 0; bb = C; }
  else              { rr = C; gg = 0; bb = X; }
  r = (uint8_t)((rr + m) * 255.0f);
  g = (uint8_t)((gg + m) * 255.0f);
  b = (uint8_t)((bb + m) * 255.0f);
}

static void parseReply(const uint8_t *buf, size_t len) {
  if (len < 24) return;
  size_t plen = (buf[12] << 24) | (buf[13] << 16) | (buf[14] << 8) | buf[15];
  if (plen < 8 || 16 + plen > len + 8) return;
  size_t bodyLen = plen - 8;
  const uint8_t *body = buf + 16;

  // Some replies carry a return code first, some the version header.
  if (bodyLen > 4 && body[0] == 0 && body[1] == 0 && body[2] == 0) {
    body += 4;
    bodyLen -= 4;
  }
  if (bodyLen > 15 && memcmp(body, "3.3", 3) == 0) {
    body += 15;
    bodyLen -= 15;
  }
  if (bodyLen == 0 || bodyLen % 16) return;

  uint8_t plain[512];
  size_t n = aesDecrypt(body, bodyLen > 512 ? 512 : bodyLen, plain);
  if (!n) return;
  plain[n] = '\0';

  JsonDocument doc;
  if (deserializeJson(doc, (const char *)plain)) return;
  JsonVariant dps = doc["dps"];
  if (dps.isNull()) return;

  if (!dps["20"].isNull()) g_state.power = dps["20"].as<bool>();

  // dp 24 is "hhhhssssvvvv". Reading it back is what lets a long-press capture
  // a look that was set from the phone rather than from this screen.
  const char *col = dps["24"] | (const char *)nullptr;
  if (col && strlen(col) == 12) {
    char part[5] = {0};
    memcpy(part, col, 4);      uint16_t h = strtol(part, nullptr, 16);
    memcpy(part, col + 4, 4);  uint16_t sat = strtol(part, nullptr, 16);
    memcpy(part, col + 8, 4);  uint16_t val = strtol(part, nullptr, 16);
    if (h <= 360 && sat <= 1000 && val <= 1000) {
      hsvToRgb(h, sat, val, g_state.r, g_state.g, g_state.b);
      g_state.bright = (uint8_t)(val / 10);
    }
  }
  g_state.online = true;
}

static void queryState() {
  char json[128];
  snprintf(json, sizeof(json), "{\"gwId\":\"%s\",\"devId\":\"%s\"}", g_devId,
           g_devId);
  uint8_t reply[512];
  size_t rlen = 0;
  if (sendFrame(CMD_DP_QUERY, json, reply, &rlen)) {
    parseReply(reply, rlen);
  } else {
    g_state.online = false;
  }
}

static void sendCommand(const LightCmd &c) {
  char json[192];
  // `t` is seconds since the epoch; the device is lenient in 3.3 but sending
  // something plausible costs nothing now that NTP is set in setup().
  uint32_t t = (uint32_t)time(nullptr);
  if (c.isBool) {
    snprintf(json, sizeof(json),
             "{\"devId\":\"%s\",\"uid\":\"%s\",\"t\":%u,\"dps\":{\"%u\":%s}}",
             g_devId, g_devId, t, c.dp, c.boolVal ? "true" : "false");
  } else {
    snprintf(json, sizeof(json),
             "{\"devId\":\"%s\",\"uid\":\"%s\",\"t\":%u,\"dps\":{\"%u\":\"%s\"}}",
             g_devId, g_devId, t, c.dp, c.strVal);
  }

  uint8_t reply[512];
  size_t rlen = 0;
  bool ok = sendFrame(CMD_CONTROL, json, reply, &rlen);
  g_state.online = ok;
  if (c.isBool) {
    Serial.printf("lights: dp%u=%s %s\n", c.dp, c.boolVal ? "true" : "false",
                  ok ? "ok" : "FAILED");
  } else {
    Serial.printf("lights: dp%u=%s %s\n", c.dp, c.strVal, ok ? "ok" : "FAILED");
  }
}

// Owns every socket operation, so a slow or absent light can never stall the
// render loop - the same reason the ring has its own task.
static void lightsTask(void *arg) {
  (void)arg;
  uint32_t lastPoll = 0;
  LightCmd cmd;
  for (;;) {
    if (xQueueReceive(g_queue, &cmd, pdMS_TO_TICKS(200)) == pdTRUE) {
      sendCommand(cmd);
      lastPoll = millis();  // the command answered; no need to poll just now
    } else if (millis() - lastPoll >= POLL_INTERVAL_MS) {
      lastPoll = millis();
      queryState();
    }
  }
}

// --- api --------------------------------------------------------------------

static bool enqueue(const LightCmd &c) {
  if (!g_configured || !g_queue) return false;

  // Drop an identical repeat within 120 ms. This existed at 400 ms to absorb
  // phantom click trains, which turned out to be a coordinate bug rather than
  // anything real - at that length it also swallowed deliberate quick presses.
  // 120 ms is short enough to be invisible and long enough to stop a genuinely
  // stuck panel from machine-gunning the light.
  static uint8_t lastDp = 0xFF;
  static char lastVal[20] = {0};
  static uint32_t lastAt = 0;
  char val[20];
  if (c.isBool) snprintf(val, sizeof(val), "%d", c.boolVal ? 1 : 0);
  else snprintf(val, sizeof(val), "%s", c.strVal);

  if (c.dp == lastDp && strcmp(val, lastVal) == 0 &&
      millis() - lastAt < 120) {
    return true;  // silently absorbed; the state is already what was asked for
  }
  lastDp = c.dp;
  strncpy(lastVal, val, sizeof(lastVal) - 1);
  lastAt = millis();

  return xQueueSend(g_queue, &c, 0) == pdTRUE;
}

void lightsBegin(const char *ip, const char *devId, const char *localKey) {
  if (ip) strncpy(g_ip, ip, sizeof(g_ip) - 1);
  if (devId) strncpy(g_devId, devId, sizeof(g_devId) - 1);
  if (localKey && strlen(localKey) == 16) memcpy(g_key, localKey, 16);

  g_configured = g_ip[0] && g_devId[0] && (localKey && strlen(localKey) == 16);
  if (!g_configured) {
    Serial.println("lights: no local key set - UI live, transport disabled");
    return;
  }

  presetsLoad();
  g_queue = xQueueCreate(8, sizeof(LightCmd));
  xTaskCreatePinnedToCore(lightsTask, "lights", 8192, nullptr, 1, nullptr, 0);
  Serial.printf("lights: Tuya 3.3 client for %s (%s)\n", g_ip, g_devId);
}

void lightsPoll() {
  // Everything happens on the task; nothing to do from the loop.
}

bool lightsSetPower(bool on) {
  g_state.power = on;  // optimistic, so the UI responds immediately
  LightCmd c = {20, true, on, ""};
  return enqueue(c);
}

// Brightness rides in the V field of the colour datapoint: this unit has no
// dp 22, and refuses writes to dp 21, so dp 24 is the only way in.
static bool sendColour() {
  uint16_t h, s, v;
  uint8_t r = g_state.r, g = g_state.g, b = g_state.b;
  uint8_t mx = max(r, max(g, b)), mn = min(r, min(g, b));
  int d = mx - mn;

  float hue = 0;
  if (d) {
    if (mx == r) hue = 60.0f * fmodf((float)(g - b) / d, 6.0f);
    else if (mx == g) hue = 60.0f * (((float)(b - r) / d) + 2.0f);
    else hue = 60.0f * (((float)(r - g) / d) + 4.0f);
  }
  if (hue < 0) hue += 360.0f;

  h = (uint16_t)hue;
  s = mx ? (uint16_t)(1000.0f * d / mx) : 0;
  v = (uint16_t)(g_state.bright * 10);  // 0-100 -> 0-1000
  if (v < 10) v = 10;

  LightCmd c = {24, false, false, ""};
  snprintf(c.strVal, sizeof(c.strVal), "%04x%04x%04x", h, s, v);
  return enqueue(c);
}

bool lightsSetBrightness(uint8_t percent) {
  if (percent > 100) percent = 100;
  g_state.bright = percent;
  return sendColour();
}

bool lightsSetColour(uint8_t r, uint8_t g, uint8_t b) {
  g_state.r = r;
  g_state.g = g;
  g_state.b = b;
  return sendColour();
}

bool lightsScene() {
  LightCmd c = {21, false, false, ""};
  strncpy(c.strVal, "scene", sizeof(c.strVal) - 1);
  return enqueue(c);
}

// --- presets ----------------------------------------------------------------

struct Preset {
  bool used;
  bool power;
  uint8_t r, g, b, bright;
};
static Preset g_presets[LIGHT_PRESETS];
static Preferences g_prefs;

static void presetsLoad() {
  g_prefs.begin("lights", true);
  for (uint8_t i = 0; i < LIGHT_PRESETS; i++) {
    char key[8];
    snprintf(key, sizeof(key), "p%u", i);
    size_t n = g_prefs.getBytes(key, &g_presets[i], sizeof(Preset));
    if (n != sizeof(Preset)) {
      // Never saved: seed with something immediately useful. The fixed colour
      // swatches were removed because they collided with this row, so an empty
      // slot would leave no way to set a colour without reaching for a phone.
      static const uint8_t SEED[LIGHT_PRESETS][3] = {
          {255, 60, 40}, {60, 255, 90}, {70, 130, 255}};
      g_presets[i].used = true;
      g_presets[i].power = true;
      g_presets[i].r = SEED[i][0];
      g_presets[i].g = SEED[i][1];
      g_presets[i].b = SEED[i][2];
      g_presets[i].bright = 80;
    }
  }
  g_prefs.end();
}

bool lightsSavePreset(uint8_t slot) {
  if (slot >= LIGHT_PRESETS) return false;
  Preset &p = g_presets[slot];
  p.used = true;
  p.power = g_state.power;
  p.r = g_state.r;
  p.g = g_state.g;
  p.b = g_state.b;
  p.bright = g_state.bright;

  g_prefs.begin("lights", false);
  char key[8];
  snprintf(key, sizeof(key), "p%u", slot);
  g_prefs.putBytes(key, &p, sizeof(Preset));
  g_prefs.end();
  Serial.printf("lights: preset %u saved (%02X%02X%02X @ %u%%)\n", slot, p.r,
                p.g, p.b, p.bright);
  return true;
}

bool lightsRecallPreset(uint8_t slot) {
  if (slot >= LIGHT_PRESETS || !g_presets[slot].used) return false;
  const Preset &p = g_presets[slot];
  g_state.bright = p.bright;
  if (!p.power) return lightsSetPower(false);
  lightsSetPower(true);
  return lightsSetColour(p.r, p.g, p.b);
}

bool lightsPresetUsed(uint8_t slot) {
  return slot < LIGHT_PRESETS && g_presets[slot].used;
}

void lightsPresetColour(uint8_t slot, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (slot >= LIGHT_PRESETS) { r = g = b = 60; return; }
  r = g_presets[slot].r;
  g = g_presets[slot].g;
  b = g_presets[slot].b;
}

const LightState &lightsState() { return g_state; }

bool lightsConfigured() { return g_configured; }
