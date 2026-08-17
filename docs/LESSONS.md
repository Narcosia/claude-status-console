# Lessons

Things this project cost real time to learn, written down so they only cost it
once. Most are not about embedded systems; they are about believing the wrong
thing for too long.

The single most expensive pattern, by a wide margin:

> **A diagnostic that cannot distinguish between its candidates is worse than no
> diagnostic, because it manufactures confidence.**

It happened four separate times below.

---

## 1. Three diagnostics that lied

**The GPIO probe that proved nothing.** The LED ring was dark and the vendor pin
map named three candidate pins. To avoid reflashing three times, a probe drove
all three at once. The ring lit — and the probe was structurally incapable of
saying *which pin* did it. That false confidence sent the investigation toward
wiring and power for an hour. `POST /ringscan`, which walks the pins one at a
time, answered it immediately: GPIO18. Waveshare's published map is wrong.

**The serial capture that was never connected.** Several rounds of touch
debugging returned "zero touches registered". The capture script never asserted
DTR, and the firmware calls `Serial.setTxTimeoutMs(0)`, so the device discards
output when no host looks attached. The device was fine and talkative; the
listener was deaf. Asserting DTR produced the answer in twelve seconds. This is
why `tools/serial-capture.py` exists and why its docstring leads with DTR.

**The edit that silently did not apply.** A `python -c` string replacement
failed to match, so a carefully-designed before/after comparison flashed
identical firmware twice. The contradictory readings looked like a hardware
mystery. Now: every scripted edit asserts its match (`if old not in s: raise`),
and a build is not "flashed" until the size line and the image hashes are
found in the log.

**The check that failed toward success.** `grep -q "Failed uploading" log ||
echo "upload ok"` prints *upload ok* when the log file does not exist. A
mistyped script name meant nothing was built at all, and the check reported
success. Meanwhile a health-probe loop printed nothing on failure, so a dead
device looked like a slow one. Both failed toward "fine".

> Write assertions positively: require the evidence to be *found*, and say so
> loudly when it is not. `tools/verify-flash.sh` is that rewritten.

---

## 2. The inference that was never re-checked

The touch panel appeared to report Y inverted. A flip was added:
`lastY = (LCD_HEIGHT - 1) - ys[0]`.

It was wrong. `setMirrorXY(true, true)` **is** applied inside the driver, higher
up than the file that had been grepped. The manual flip was a second correction
on top of the first, which made the entire lower half of the screen unreachable
— every control below the centre silently dead.

Four rounds of fixes were then built on top of that inference: bigger targets,
different event types, double-tap, on-screen pills. Each partly helped, which
made the underlying error *harder* to see. The README documented the wrong
conclusion as established fact for a while, which is worse than not documenting
it at all.

> When a fix "sort of works", suspect the diagnosis, not the dose. And re-derive
> an inference before building a fourth thing on top of it.

---

## 3. The error message that was telling the truth

Every cloud call for the light returned `permission deny`. This was read as an
auth problem, and led to a data-centre change the user did not need to make.

The device being asked about simply was not in that account. There were two
Tuya devices on the network; the assumed one (.242, protocol 3.4) belonged
elsewhere, and the actual light bar was .172 on protocol 3.3. The API was
answering the question correctly the whole time.

> Identify the device before debugging the conversation with it.
> `tools/tuya-discover.py` does that in twenty seconds.

---

## 4. Instrumentation that caused the bug it was measuring

Frame-timing prints were added to find a display stutter. Serial here is USB
CDC with no UART bridge: a write blocks until the host drains it, or until its
timeout expires when nothing is listening. A print every five seconds produced
exactly the symptom being chased — smooth for a few seconds, then a pause.

Later, the same prints ate touch input: enough blocking in the input path to
drop swipes. Every debug print is now behind `UI_PROFILE` or `TOUCH_DEBUG`,
both default 0.

> On this board, printing is not free and not passive.

---

## 5. A page that only a finger can test

A 40 px glow was added to a 260 px power button for looks. It compiled, flashed,
verified two image hashes, and answered an HTTP health check. Then it hung the
device dead — no panic, no reboot, no serial — the instant the page was opened.

LVGL sizes its shadow mask by `(shadow_width + radius)²  × 2` bytes.
`LV_SHADOW_CACHE_SIZE` is 0 (recomputed every draw), `LV_MEM_SIZE` is 48 KB,
and `LV_USE_ASSERT_MALLOC`'s failure handler is `while(1);`. A radius-130
circle asks for 35 KB from a pool with 31.8 KB free. It never comes back.

The first fix removed the explicit shadow and changed nothing, because
`lv_btn_create` inherits `shadow_width LV_DPX(3)` from the default theme — a
button has a shadow whether or not you asked for one. That cost a second
flash-and-freeze cycle in front of the user.

Full arithmetic in [DISPLAY-PERFORMANCE.md](DISPLAY-PERFORMANCE.md). Two rules
came out of it:

- **No shadows on large objects.** State on anything big is carried by border
  width and colour, which cost nothing.
- **`uiSelfTest()`** now renders every page at boot and prints LVGL pool usage.
  A page that renders only on demand can only be tested by a fingertip, and
  every automated check will pass while the device sits one touch from a hang.

---

## 6. Hardware documentation is a hypothesis

Vendor material for this board has been wrong four times: the H2 pin map,
`I2S_MCK_IO`, and two touch-driver assumptions. The Tuya cloud DP spec for the
light is wrong in three places — dp 21 rejects writes, dp 22 does not exist, and
dp 24 overrides scene playback.

`board.h` and the datapoint table in the README record what was *measured*, and
say where they contradict the vendor.

> Measure it, then write down what you measured and how.

---

## 7. Constraints are design input, not obstacles

The lights page ended up as a single power button. Not because more was too
hard — colour swatches, NVS presets and a scene trigger were all built and all
worked at the protocol level — but because this unit has no dp 22, so
brightness and colour both travel in dp 24, and writing dp 24 stops the light's
own animation. A dim control that kills the scene is worse than no dim control.

Likewise the presets: 50 px targets and a hold-to-save gesture were the wrong
interaction for this panel, which needs large targets and one unambiguous tap.
Two controls that always work beat six that need a careful finger.

> Build what the device can actually do well, then remove what it cannot.
