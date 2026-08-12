# Claude Code Status Console

A round AMOLED and a 24-LED ring, driven from one device, showing which of your
Claude Code sessions are working and which have stopped and are waiting on you.

This is the successor to [claude-status-ring](../claude-status-ring): same
hooks, same protocol, same semantics, moved from MicroPython to Arduino/C++
because the display on this board needs it. The ring now hangs off the new
board's expansion header, so there is one device instead of two.

Each live session owns an arc. The screen draws that arc at the same angle as
the physical one and animates it on the same phase counter, so the two surfaces
read as one object rather than two displays that happen to agree.

| State | Appearance (default `vapor` theme) | Meaning |
|---|---|---|
| **working** | cyan, quick comet | actively processing your turn |
| **background** | violet, slower comet | agents running — wants nothing from you |
| **input** | hot pink, slow pulse | turn finished — awaiting your next prompt |
| **attention** | pink-red, fast pulse | permission prompt or explicit request for input |
| **done** | white, fading out | session ended |
| *no sessions* | single dim pixel creeping round, `IDLE` on screen | idle, but powered and connected |

**Motion carries the same information as colour, independently.** Comets mean
"leave it alone", pulses mean "it wants you", faster means more urgent within
each pair. Both renderers read their timing from `motion.h` for exactly this
reason: a session breathing at one rate on the ring and another on the screen
would read as two different states.

### The screen

The centre answers *does anything want me?* before any text is read — the
headline count takes the colour of the most urgent session — with the
breakdown underneath:

```
        3
     AGENTS
1 needs you  2 working
```

**Tap an arc** for that session's card:

```
     ring firmware            <- label, or the directory name
  ~/claude-status-console      <- path, so same-named dirs differ
 "fix the arc alignment on
     the round display"        <- first line of the current prompt
      working
   aaaa1111  2m  3 agents
```

Tap again to dismiss; it also times out after 15 seconds. Touches are resolved
by angle from the centre rather than by hit-testing the arc widgets, because
arc bounding boxes all overlap in the middle and would fight over the press.

### The event horizon (built, currently off)

Behind the summary, seven concentric ripples travel outward and nine chevrons
sit around the inner edge — one locks per active session. With nothing running
the gate is dormant: the ripples drop to a faint glow and every chevron stays
dark, so "idle" still reads as idle at a glance.

Two decisions kept it from costing anything that matters:

**The chevrons take the colour of the most urgent session, not the canonical
amber.** Warm means "wants you" everywhere else on this device. A decorative
amber that meant nothing would break the one rule the whole colour scheme
rests on.

**The gate redraws at a third of the UI rate.** Each ripple is a full circle,
so its invalidation covers the whole centre disc — animating that at 30 fps
would push a near-full-screen flush over QSPI every frame, competing with WiFi
and the hook server. An event horizon ripples slowly anyway.

**`STARGATE` is currently `0`** — the effect is built and kept in `ui.cpp`, but
compiled out. Even with the display path fixed, a full-screen composite is
~43 ms of LVGL rendering and the gate roughly doubles it, which is more than
this panel can animate smoothly alongside the session arcs.

The whole investigation is written up in
[docs/DISPLAY-PERFORMANCE.md](docs/DISPLAY-PERFORMANCE.md): the measurements,
the two real causes, the optimisation that made no difference at all, and what
to try before turning it back on. Worth reading before touching the render
path — the baseline was 11 fps long before any of this was added.

### Naming sessions

Directory names are often too generic to tell sessions apart — three sessions
in the same repo all read `dotfiles`. Two things fix that.

**The topic line** does it automatically. It is the session's **first** prompt,
condensed, and it never changes afterwards — several sessions in one directory
share a name and a path, so the only thing separating them is what each was
asked to do, and that has to stay put rather than following every turn. A
second `now:` line appears while a session is actually running, if it has moved
on from what it started with.

Prompts are condensed rather than summarised — the device cannot run a model.
Filler openers are stripped, the first sentence kept, and the tail cut on a
word boundary:

```
"Hey, can you please look at why the arc alignment is off on the
 round display? I think it might be the offset..."
        ↓
"look at why the arc alignment is off on the round display"
```

Real summarisation would mean either an API key on the device and your prompts
leaving the LAN, or a `command` hook that shells out to a model — reintroducing
the wrapper script this design exists to avoid. Condensing recovers most of the
value for neither cost.

**An explicit label** does it deliberately. Claude Code's `http` hook takes a
plain URL, so a query string works — put this in a project's
`.claude/settings.json`:

```json
{"type": "http", "url": "http://192.168.1.200/hook?label=ring%20firmware", "timeout": 5}
```

Every session started in that directory then names itself `ring firmware`.

Project and user hooks *both* fire, so the device only ever **sets** a label
when one is present and never clears it: the labelled POST wins and sticks,
and the global hook installed by `install-hooks.sh` keeps working untouched.
No wrapper script, no per-machine bookkeeping.

`agent_type` is not a substitute here — it is only populated for subagents and
for sessions started with `--agent`, so it cannot name an ordinary session.

### Themes

Set `THEME` in `config.h`. It drives the ring and the screen together.

| Theme | working / background / input / attention |
|---|---|
| `vapor` *(default)* | cyan / violet / hot pink / pink-red |
| `neon` | cyan / spring green / hot pink / red |
| `classic` | blue / green / amber / red |

`vapor` is the prettiest and the least legible; `classic` is the most legible
and the dullest. An unknown name logs a warning and falls back to `classic`
rather than raising — a typo in config should dim the lights, not take an
unattended device's HTTP server down on boot.

## How it works

```
Claude Code session ──HTTP POST──► ESP32-S3 ──► AMOLED (LVGL)
     (hooks)                                └─► WS2812B ring
```

Claude Code supports a native `"type": "http"` hook that POSTs the event JSON
straight to a URL, so **the device is the webhook endpoint directly** — no
wrapper script, no daemon, no USB tether.

| Hook event | → effect |
|---|---|
| `SessionStart`, `UserPromptSubmit` | working (and resets the pending counter) |
| `Stop` | input |
| `Notification` (`permission_prompt`, `agent_needs_input`, `elicitation_dialog`) | attention |
| `Notification` (`idle_prompt`, `agent_completed`) | input |
| `SessionEnd` | done |
| `SubagentStart`, `TaskCreated` | pending + 1 |
| `SubagentStop`, `TaskCompleted` | pending − 1 |

### Background work vs waiting on you

A session that spawns background agents stops emitting foreground events. Left
alone that looks identical to a session sitting idle, so the device would
report it as wanting your attention when it is simply working. Two mechanisms
prevent that, both carried over unchanged:

**`idle_prompt` is not red.** It means "this session has been quiet a while",
which is exactly what background work looks like from outside. Red is reserved
for notifications that genuinely block.

**A recency window, not a counter.** The obvious design — increment on
`SubagentStart`, decrement on `SubagentStop` — does not survive contact with
reality: in the field `SubagentStart` was observed firing **once against eleven
`SubagentStop`s**. So *any* background event refreshes a `BACKGROUND_HOLD_MS`
window (90 s default). The counter is kept as corroboration, never as the sole
signal. The window is cleared on `UserPromptSubmit`.

`attention` overrides the window entirely.

## Hardware

| Part | Detail |
|---|---|
| Board | Waveshare ESP32-S3-Touch-AMOLED-1.75 |
| Chip | ESP32-S3R8 rev v0.2, 16 MB flash, 8 MB octal PSRAM |
| Display | CO5300 AMOLED, 1.75″, 466×466, QSPI |
| Touch | CST9217 capacitive, I²C `0x5A` |
| Ring | Jaycar XC4385, 24× WS2812B, 72 mm, 5 V |

### Wiring the ring

The board's 8-pin 2.54 mm header (`H2`) is the only exposed GPIO:

| H2 pin | Signal | Ring |
|---:|---|---|
| 1 | VBUS | `VCC` |
| 2 | GND | `Gnd` |
| 3 | 3V3 | — |
| 4 | GPIO44 / U0RXD | — |
| 5 | GPIO43 / U0TXD | — |
| 6 | GPIO16 | — |
| 7 | GPIO17 | — |
| 8 | **GPIO18** | `Din` |

**That GPIO order is measured, and contradicts `HARDWARE_REFERENCE.md`**, which
lists pins 6/7/8 as GPIO17/GPIO18/**GPIO16**. The non-sequential ordering in the
official table is the tell — the pins run 16, 17, 18 across holes 6, 7, 8, so
the last hole is GPIO18.

Established with `POST /ringscan`, which drives each candidate pin in its own
colour and lets the ring identify itself. If a ring in the last hole lights
**blue**, the mapping above holds; **red** would mean the documented mapping is
right for your revision. Check rather than assume — this is the second place
this board's documentation disagrees with its hardware, after `I2S_MCK_IO`.

Leave the ring's `Dout` triplet unconnected — it is only for daisy-chaining.

The header is **female**, so this needs male-ended jumper wires — the opposite
of what the old Keyestudio board took.

### Finding pin 1

The unit is enclosed, so the `H2` silkscreen is not visible. Identify the end
electrically rather than by counting from a guess, with the board USB-powered
and the ring not yet connected:

| Probe | Expect |
|---|---|
| pin 1 → pin 2 | ~5 V (VBUS) |
| pin 3 → pin 2 | 3.3 V |
| pin 8 → pin 2 | ~0 V, floating (GPIO16) |

If the 5 V and 3.3 V readings land on pins 8 and 6 instead, you are counting
from the wrong end and the row is reversed. Getting this wrong puts 5 V into a
GPIO that is not 5 V tolerant, so it is worth the two minutes.

**Watch out for Waveshare's own `pin_config.h`**: it defines `I2S_MCK_IO 16`,
which contradicts both the schematic and the maintained BSP — audio MCLK is
GPIO 42, and GPIO 16 is a free expansion pin. Of all the defines to get wrong,
that is the one that matters here, so `board.h` in this repo is self-contained
rather than including theirs. Nothing in this firmware touches the audio path.

**Levels.** The header is 3.3 V and *not* 5 V tolerant. The ESP32-S3 drives ring
data at 3.3 V while a WS2812B at 5 V wants ≥3.5 V for a logic high. It works,
as it did on the old board, but flickering or a misbehaving first LED points at
that margin rather than at the code.

**Power.** 24 WS2812Bs at full white draw ~1.4 A, and here they share VBUS with
an AMOLED and a WiFi radio. `RING_BRIGHTNESS` defaults to 24 (~9%), keeping the
whole ring near 150 mA at pulse peak. Raise it only with a dedicated 5 V supply
whose ground is tied to the board's.

**Aligning the arcs.** `RING_ZERO_DEG` in `ui.h` is the angle of LED 0, in LVGL
degrees (0 = 3 o'clock, clockwise). It defaults to 270, putting LED 0 at 12
o'clock. Set it once after mounting so a session's arc on screen points at its
arc on the ring.

## Setup

**1. Libraries.** The board-specific ones are vendored by Waveshare and are not
in the Arduino library index — copy them out of
[their repo](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75)
(`examples/arduino/libraries/`) into `~/Arduino/libraries/`:

```
GFX_Library_for_Arduino   ESP32_IO_Expander   SensorLib
lvgl (8.4) + lv_conf.h    esp-lib-utils       XPowersLib
```

`lv_conf.h` goes in `~/Arduino/libraries/` itself, beside the `lvgl` folder.
The rest come from the index:

```sh
arduino-cli core install esp32:esp32
arduino-cli lib install "Adafruit NeoPixel" ArduinoJson
```

**2. Configure:**

```sh
cp config.example.h config.h
$EDITOR config.h        # WiFi credentials, static IP
```

`config.h` is gitignored. Do not commit it.

A static IP is worth setting: the hook URL in `settings.json` is a fixed
string, so a DHCP lease change would silently break the device.

**3. Flash:**

```sh
./deploy.sh /dev/ttyACM0
```

**4. Wire up the hooks:**

```sh
./install-hooks.sh 192.168.1.200
```

Merges the nine hooks into `~/.claude/settings.json`, preserving everything
already there and writing a timestamped backup first. Safe to re-run — it
replaces any hook whose URL ends in `/hook`, which means it also takes over
cleanly from the old ring at `.200`. `./install-hooks.sh --remove` undoes it.

**5. Verify** without involving Claude Code:

```sh
curl -s http://192.168.1.200/                 # current sessions as JSON
curl -sX POST http://192.168.1.200/hook -H 'Content-Type: application/json' \
  -d '{"session_id":"test-1","hook_event_name":"Stop","cwd":"/home/you/demo"}'
# one pink arc should start pulsing, labelled "demo" when tapped
curl -sX POST http://192.168.1.200/clear      # forget everything
```

## Multiple machines

Hooks are per-machine configuration — there is no central registry, so **every
computer running Claude Code needs its own hook config**. The device side needs
no changes at all: it accepts any `session_id` from any source, and session ids
are UUIDs, so sessions from different machines cannot collide. They simply
appear as additional arcs.

On each additional machine, with the repo cloned:

```sh
./install-hooks.sh 192.168.1.200
curl -s http://192.168.1.200/health
```

**Windows machines work too.** Paths are parsed with both separators, so
`C:\Users\me\Documents\Vault` names itself `Vault` rather than the front of the
path truncated, and both home prefixes collapse to `~` — `/home/<user>/` and
`C:\Users\<user>\` — since they are identical for every session on a machine
and only consume the width that distinguishes one project from another.

Requirements for a machine to participate:

- **Same LAN.** The device is on a private address. A laptop on mobile data or
  a different network cannot reach it.
- **Do not port-forward the device to the internet.** Hook payloads contain
  prompt text and file paths, there is no TLS, and `HOOK_TOKEN` travels in
  cleartext. Use a VPN or an overlay network such as Tailscale instead.

**Telling machines apart:** arcs carry no per-machine identity, so with sessions
from several computers you can see *that* something wants attention but not
*which* machine it is on. Give that machine's hook URL a `?label=`, or read the
`path` field — a `C:\`-rooted path is unambiguous on sight.

## Endpoints

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/hook` | hook payload from Claude Code |
| `GET` | `/` | session states as JSON |
| `GET` | `/health` | liveness check |
| `GET` | `/events` | last 40 events (metadata only) for diagnosis |
| `POST` | `/clear` | forget all sessions |
| `POST` | `/ringtest` | 15 s: every LED cycling red/green/blue, bypassing all arc logic |
| `POST` | `/ringzero` | 180 s: LED 0 white with a fading trail, quarter marks in blue |

`/ringtest` answers "is any signal reaching the chain" without the session
state, the palette or the brightness scaling in the way — the question that
matters when the ring is dark and everything else looks healthy. `/ringzero`
answers "where is LED 0 and which way do the indices run", which is what
`RING_ZERO_DEG` needs.

`/hook` always answers **200 with an empty body**. This is deliberate: Claude
Code treats a 2xx with a text body as *added context*, feeding it back into the
conversation. A status light must never inject text into your session.

`GET /` reports `state` (what is drawn) beside `raw_state` (the last event's
state), `pending` and `expires_in_ms`, so a surprising colour can be traced to
the event that caused it.

## What this device knows

Hook payloads contain far more than session state — `user_input`,
`last_assistant_message`, `message`, `cwd` and `transcript_path` all arrive in
the POST body.

**Read and kept:** `session_id`, `hook_event_name`, the notification subtype,
`cwd` (both the basename and a `~`-collapsed tail of the full path), and a
condensed first sentence of **`user_prompt`** and of
**`last_assistant_message`**. Nothing else.

**Still discarded:** `message`, `tool_input`, `tool_result`, `transcript_path`,
and everything past the first sentence of the two message fields.

That boundary is enforced *during parsing*, not after it. The body is streamed
off the socket through an ArduinoJson filter that admits only those keys, so
assistant replies are skipped as they arrive and are never allocated at all.
This is also why the HTTP server is hand-rolled on `WiFiServer` rather than
using `WebServer`, which buffers the whole body into a `String` before any
handler sees it.

**On admitting the two message fields.** This is the largest deliberate step
past what the old ring kept, and the reasoning is worth stating plainly.

A directory name cannot distinguish three sessions in the same repo; what each
was *asked to do* can. And a session sitting in `input` has no recent prompt at
all — it has a last reply — so without `last_assistant_message` the state you
look at most often is exactly the one that renders blank.

Both fields change what is *visible on a desk*, not what travels: they are in
every `UserPromptSubmit` and `Stop` payload already and always were, and the
filter only decides whether to keep them. **The screen is the exposure here,
not the network.** If the display sits where other people do, drop
`user_prompt` and `last_assistant_message` from the filter in `server.cpp` —
the card falls back to the label and path, and everything else still works.

Two things to be aware of regardless:

- **The payloads cross your LAN in plaintext.** An ESP32 is not a realistic TLS
  terminator, so anything with visibility of your network segment could read
  prompt text in transit — the filtering happens at this end, after the wire.
  On a home network this is typically fine; on a shared or untrusted one it is
  not.
- **`HOOK_TOKEN` is a light guard, not security.** It stops another device on
  the network from casually driving your lights. Since it travels in a
  plaintext header, it does not protect the payloads themselves.

Do not port-forward this device to the internet. For a machine outside the LAN,
use a VPN or an overlay network such as Tailscale.

## Reliability

- **Device powered off:** hook connections are refused immediately and Claude
  Code treats that as a non-blocking error. Sessions are unaffected. The
  installed hooks use `timeout: 5` (2 for the chatty background events) rather
  than the 600 s default, so an unreachable device cannot stall a turn.
- **Sessions that die without `SessionEnd`:** expiry is state-dependent. A
  session still marked `working` that goes quiet for `WORKING_TTL_MS` (1 h) is
  presumed dead. Sessions in `input` or `attention` get `SESSION_TTL_MS` (4 h),
  because they can legitimately sit for a long time waiting on a human, and
  that is exactly the state worth showing. Pruning is safe either way: an
  unknown `session_id` is registered on arrival, so a wrongly-pruned session
  reappears the moment it emits any event.
- **More than 16 sessions:** the table evicts the least urgent, oldest session
  rather than dropping the new one. Same self-healing applies.
- **WiFi drops:** retried every 30 s. A failed connection at boot is not fatal —
  the ring and screen still render, and the address line shows `no wifi`.
- **Long redraws:** the ring runs on its own FreeRTOS task at priority 2, so a
  full-screen LVGL flush cannot stutter it. Both renderers work from snapshots
  taken under the registry's mutex rather than holding it for a whole frame.

## Files

| File | Purpose |
|---|---|
| `claude-status-console.ino` | board bring-up, LVGL glue, tasks, main loop |
| `sessions.{h,cpp}` | session registry, expiry, ordering, background window |
| `server.{h,cpp}` | HTTP endpoints, hook classification, event log |
| `ui.{h,cpp}` | LVGL radial arcs, centre summary, tap-for-detail |
| `ring.{h,cpp}` | WS2812B rendering and per-state arc painters |
| `layout.{h,cpp}` | how the ring is divided between sessions — shared by both |
| `motion.{h,cpp}` | the animation vocabulary — shared by both |
| `board.h` | board pins, self-contained |
| `config.example.h` | template — copy to `config.h` |
| `deploy.sh` | compile and flash |
| `install-hooks.sh` | merge the hooks into `~/.claude/settings.json` |
| `stage-demo.sh` | four invented sessions, one per state, for screenshots |

`stage-demo.sh` exists because the card renders prompt text and assistant
replies, so a candid photograph of a working device publishes whatever happened
to be on it. Every name, path and prompt in that script is fabricated.

## Gotchas hit along the way

- **`esptool read-flash` fails with the stub on this board.** Reading the flash
  over USB-Serial/JTAG dies with `A fatal error occurred: Packet content
  transfer stopped`, at any baud and any chunk size. `--no-stub` works:

  ```sh
  esptool --port /dev/ttyACM0 --no-stub read-flash 0 0x1000000 factory.bin
  ```

  The `-b` flag is meaningless over native USB anyway.
- The stock firmware is `esp-brookesia`, and Waveshare ship the same image in
  their repo under `firmware/`, so the demo is restorable whether or not you
  took a backup first.
- **The AMOLED has no backlight GPIO.** Brightness is a panel command
  (`gfx->setBrightness()`); LCD backlight PWM recipes from other boards do not
  apply.
### The dark ring: an honest post-mortem

The ring stayed dark for an afternoon. **The cause was the pin map**: the
firmware drove GPIO16 because `HARDWARE_REFERENCE.md` says hole 8 is GPIO16,
and the ring was actually on GPIO18. Nothing else was ever wrong — not the
wiring, not the soldering, not the LEDs, not the render task.

It is worth recording what went wrong in the *diagnosis*, because that is what
cost the time rather than the bug itself.

**The decisive mistake was a probe that drove GPIO16, 17 and 18
simultaneously.** It was built that way so the `Din` wire could be moved
between holes without a reflash. It lit the ring — and proved only that *at
least one* of three pins worked, while being read as proof that GPIO16 worked.
Every later hypothesis was built on that false conclusion. A diagnostic that
cannot distinguish between its candidates is worse than none, because it
manufactures confidence. `POST /ringscan` is the corrected version: one pin at
a time, each in its own colour, so the ring names its own pin.

**Two red herrings, both plausible, both wrong**, kept alive by that bad probe:

- *Initialisation order.* `rmtInit()` needs internal DMA-capable RAM, and LVGL's
  two 43 KB draw buffers take most of it, so `ring.begin()` running after
  `lv_init()` looked like a starvation bug. It was never demonstrated —
  `rmtInit` on GPIO16 succeeded every time once logging was on. The ordering in
  `setup()` is kept anyway: it costs nothing and the failure mode is real, just
  not this one.
- *RMT buffer underrun.* `Adafruit_NeoPixel` hardcodes `RMT_MEM_NUM_BLOCKS_1` —
  a 48-symbol buffer for a 576-symbol frame — which really can underrun beside
  WiFi and a full-screen flush. Also not what happened here.

**What actually helped**, and is worth keeping:

- The **frame heartbeat** in `ringTask`. `1375 frames, stack headroom 5048
  bytes` while the ring sat dark is what ruled out a dead task and forced the
  search elsewhere. A dark ring and a crashed renderer are otherwise identical
  from outside.
- **Driving RMT directly** instead of through `Adafruit_NeoPixel`, whose
  `espShow()` returns *while still holding its mutex* when `rmtInit` fails,
  turning one failure into permanent silence. The driver here reports every
  failed `rmtInit` and `rmtWrite`; being able to say "zero write failures"
  with confidence is what redirected suspicion to the pin.
- Building with **`DebugLevel=error`**. Arduino's default discards `log_e`, so
  libraries can report faults into a void.
- Remembering that **the LEDs never lost power**. VBUS stays up across an
  ESP32 reset, so WS2812Bs hold their last latched frame through a reflash. A
  stale colour looks exactly like a freshly rendered one, and "still green
  after reflashing" was the observation that finally broke the deadlock.

## License

MIT — see [LICENSE](LICENSE).
