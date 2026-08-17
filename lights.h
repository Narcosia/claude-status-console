// Control for the Tuya light bar on the LAN.
//
// Deliberately a narrow interface, because the transport behind it is going to
// change: right now it is a stub that only tracks state, and it will become a
// Tuya 3.4 client (AES-ECB plus the HMAC-SHA256 session handshake) once the
// device's local key is in hand. The UI should not be able to tell.
//
// Every call is fire-and-forget and must not block the render loop. A light
// that fails to answer is a light that stays as it was; it is not worth
// stalling the display over.

#pragma once

#include <Arduino.h>

struct LightState {
  bool online;      // last command got a reply
  bool power;
  uint8_t bright;   // 0-100
  uint8_t r, g, b;
};

// `ip`, `devId` and `localKey` come from config.h. A null or empty localKey
// leaves the module in stub mode: the UI works and reports offline.
void lightsBegin(const char *ip, const char *devId, const char *localKey);

// Called from the main loop. Owns any retry/refresh timing.
void lightsPoll();

bool lightsSetPower(bool on);
bool lightsSetBrightness(uint8_t percent);
bool lightsSetColour(uint8_t r, uint8_t g, uint8_t b);

const LightState &lightsState();

// --- presets ----------------------------------------------------------------
//
// Three slots, held in NVS so they survive reboots and reflashes.
//
// A preset stores a *look* - power, colour, brightness - not a scene. This
// product exposes no scene_data datapoint, and its animated scenes are played
// internally: dp 24 sits perfectly still while one runs, so there is nothing
// to capture. Measured over twenty seconds, not assumed.
static const uint8_t LIGHT_PRESETS = 3;

// Captures whatever the light is currently showing, including a look set from
// the phone - the status poll keeps colour and brightness current.
bool lightsSavePreset(uint8_t slot);
bool lightsRecallPreset(uint8_t slot);
bool lightsPresetUsed(uint8_t slot);
void lightsPresetColour(uint8_t slot, uint8_t &r, uint8_t &g, uint8_t &b);

// Hands control back to the light's own scene engine. The one thing that
// cannot be stored in a slot, but can always be returned to: setting
// work_mode to "scene" re-triggers whichever scene the app last selected.
bool lightsScene();

// True once a local key is configured, whatever the connection is doing. The
// UI uses this to say "not configured" rather than "offline", which are
// different problems with different fixes.
bool lightsConfigured();
