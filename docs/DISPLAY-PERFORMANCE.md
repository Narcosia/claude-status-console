# Display performance, and the Stargate experiment

Written down because it took an afternoon, most of it spent fixing things that
were not the problem, and because the effect it was in service of is still in
the tree waiting to be turned back on.

## Where it started

A Stargate event horizon was added behind the centre summary — concentric
ripples plus nine chevrons — and the display began to stutter. The obvious
conclusion was that the effect was too much for the board.

That was wrong, and measuring it was the only way to find out.

## The measurements

Instrumentation lives in the sketch behind `UI_PROFILE` (default `0`). It times
`lv_timer_handler()`, and separately the flush callback, so render cost and
transfer cost can be told apart.

| Configuration | Per pass | Effective rate |
|---|---|---|
| Gate on, 4 sessions | 170 ms | ~6 fps |
| Gate **off**, 4 sessions | **85 ms** | ~11 fps |
| Gate off + arc style caching | **85 ms** | unchanged |
| + `LV_COLOR_16_SWAP 1` | 59 ms | ~16 fps |
| + work paced to the panel | **3.9 ms mean** | — |

The second row is the important one. **The baseline was already 11 fps before
any of this was added.** The event horizon did not create the problem; it
pushed a pre-existing one past the point where it could be ignored.

Splitting the 59 ms showed **16 ms flush, 43 ms LVGL compositing, and 213k
pixels per pass** — the entire 466×466 screen, every frame.

## What was actually wrong

**1. A byte-order mismatch, costing 217k CPU operations per frame.**

With `LV_COLOR_16_SWAP 0`, LVGL emits little-endian RGB565, so the flush lands
in `Arduino_ESP32QSPI::writePixels()`, which byte-swaps every pixel into a
bounce buffer before transmitting:

```c
for (uint32_t i = 0; i < l2; ++i) {
  p1 = *data++;  p2 = *data++;
  MSB_32_16_16_SET(_buffer32[i], p1, p2);
}
```

`draw16bitBeRGBBitmap` instead reaches `writeBytes()`, which hands the buffer
straight to the SPI driver. The flush callback already had both branches; LVGL
was simply producing the wrong byte order. `lv_conf.h` now sets
`LV_COLOR_16_SWAP 1`.

Note this changes the `lv_color16_t` layout — green splits across two
bitfields — so `colour.ch.green` stops compiling. Do not read components back
off an `lv_color_t`; keep the source values.

**2. The QSPI bus was at the library default of 40 MHz.** `gfx->begin(80000000)`
halves the transfer. Worth doing, but it was never the bottleneck — the flush
is only ~27% of a frame.

**3. Asking 30 fps of a 59 ms frame.** This is what the eye actually sees as
lurching: calls arrive faster than they complete, so the visible cadence is
whatever is left over. Work is now divided down — arcs and centre every third
pass, the gate every sixth and offset from them so they never land together.

**4. The profiling prints were themselves causing a stutter.** Serial on this
board is USB CDC; there is no UART bridge. A write blocks until the host drains
it, or until its timeout expires when nothing is listening. Printing every five
seconds produced exactly the reported symptom: smooth for a few seconds, a
pause, then smooth again. Hence `UI_PROFILE`, off by default.

## What did not work

**Caching arc styles to skip redundant LVGL writes: zero difference.** 85 ms
before, 85 ms after. The reasoning was sound — in LVGL an angle change
invalidates only the moved sector, while a *style* change invalidates the whole
bounding box, and these arcs are 448×448 — but it was not where the time was
going. The cache is kept because it is correct and free, not because it helped.

The lesson is the ordering: measure, then optimise. Two rounds of work were
spent on plausible theories before anything was instrumented.

## Why the gate is off

Even with everything above fixed, a full-screen composite is ~43 ms of LVGL
rendering, and the gate roughly doubles it. Two pulsing arcs alone invalidate
their full bounding boxes, LVGL merges those into one area, and the whole
screen redraws regardless.

The effect is preserved in `ui.cpp` behind `#if STARGATE` (see `ui.h`). It
works; it is simply more than this panel can animate smoothly alongside the
session arcs.

## If picking it up again

In rough order of expected value:

1. **Keep the pool, drop the shimmer.** Draw the horizon once and animate only
   the kawoosh. A static gradient costs nothing, and the burst is the part
   worth having. This was the option not tried.
2. **Stop the arcs invalidating the full screen.** They are 448×448 objects; a
   pulse is a style change and therefore a full-bbox invalidation. Representing
   a pulse some other way — or accepting a lower pulse rate — would cut the
   baseline for everything.
3. **A canvas for the centre.** LVGL 8 has no radial gradient, which is why the
   pool is stacked discs. Rendering it once into a canvas would trade RAM for
   render time; a 300×300 canvas at 16 bpp is ~180 KB, which does not fit
   beside the two LVGL draw buffers without moving those to PSRAM first.
4. **LVGL 9** has radial gradients natively.

## Turning it back on

```c
// ui.h
#define STARGATE 1     // event horizon and chevrons
#define UI_PROFILE 1   // frame timing on serial - attach a monitor that reads
```

`POST /kawoosh` fires the unstable vortex on demand; it also fires at boot and
whenever a new session appears.

## Shape of the effect, as built

- Six filled discs stacking to a bright cyan-white core, deep blue at the rim
- Nine chevrons in prop orange, one lit per active session
- A ~1.15 s kawoosh: erupt (0–0.34), flush (0.34–0.58), retract (0.58–0.80),
  collapse into the horizon (0.80–1.0), shaped in `vortexFront()`
- During a burst the session arcs freeze, which shrinks the invalidated area to
  the gate alone — the one thing that made the surge smooth

## Shadows: a hard hang, not a slowdown

Adding a 40 px glow to the 260 px power circle **hung the device dead** — no
panic, no reboot, no serial, no HTTP. It booted, it flashed, it answered a
health check, and it froze on the first touch that loaded that page.

Three settings in `lv_conf.h` combine to make this fatal rather than merely
slow:

| Setting | Value | Consequence |
|---|---|---|
| `LV_MEM_SIZE` | 48 KB | the whole LVGL pool |
| `LV_SHADOW_CACHE_SIZE` | 0 | shadow masks are recomputed every draw |
| `LV_USE_ASSERT_MALLOC` | 1 | and LVGL's assert handler is `while(1);` |

The exact request is in `lv_draw_sw_rect.c`:

```c
int32_t corner_size = dsc->shadow_width + r_sh;          // r_sh = corner radius
sh_buf = lv_mem_buf_get(corner_size * corner_size * sizeof(uint16_t));
```

So it is **(shadow_width + radius)² × 2 bytes**, and the radius dominates. The
danger is not the blur setting but the *size of the object wearing it*. Measured
free pool on this device at boot is 31,836 bytes, largest block 31,824:

| Object | radius | shadow | request | vs 31,824 |
|---|---:|---:|---:|---|
| nav pill 150×54 | 27 | 24 | 5,202 | fits |
| swatch 50×50 | 25 | 18 | 3,698 | fits |
| power circle 150px | 75 | 3 (theme) | 12,168 | fits |
| power circle 260px | 130 | 3 (theme) | **35,378** | **hangs** |
| power circle 260px | 130 | 40 (mine) | **57,800** | **hangs** |

A `LV_RADIUS_CIRCLE` object carries a radius of half its width, so a big circle
is the worst possible shadow target. The ceiling here is `corner_size ≤ 126` —
a circular button of about **246 px**. The 260 px one missed it by 14 pixels.

Note the third row: `lv_btn_create` inherits `shadow_width LV_DPX(3)` from the
default theme, so a button has a shadow whether or not you asked for one.
Removing an explicit shadow does not remove that; it has to be zeroed. That
mistake cost an extra flash-and-freeze cycle — the first fix looked complete
and changed nothing.

The allocation fails, `LV_ASSERT_MALLOC` spins in `while(1)`, and the symptom
is a frozen screen rather than an out-of-memory error.

**Rule:** shadows only on small objects. State on anything large is carried by
border width and colour, which cost nothing.

Worth noting the failure mode itself: every automated check passed. Compile,
flash, hash verify, and an HTTP health probe all went green while the device
was one touch away from locking up. A page that renders only when a finger
lands on it can only be tested by a finger.

Hence `uiSelfTest()`, called at the end of `setup()`: it loads each page and
forces a synchronous `lv_refr_now()`, printing LVGL pool figures around each.
A page that cannot be drawn now hangs at boot, after a line naming it, where
serial shows it — instead of hanging later under a fingertip with nothing on
the wire. It is also what turned "shadows are probably too expensive" into the
arithmetic above.
