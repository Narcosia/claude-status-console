// Claude Code status console.
//
// Receives Claude Code hook events over HTTP and shows the state of every live
// session two ways at once: as arcs on a 466x466 round AMOLED, and as arcs on
// a 24-LED WS2812B ring wired to the expansion header. Both are driven from
// one session registry and one animation phase, so they never disagree.
//
// Board: Waveshare ESP32-S3-Touch-AMOLED-1.75 (ESP32-S3R8, 16 MB flash,
// 8 MB PSRAM). See board.h for pins and README.md for wiring.

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <lvgl.h>

#include "Arduino_GFX_Library.h"
#include "TouchDrvCSTXXX.hpp"
#include "board.h"
#include "config.h"
#include "motion.h"
#include "ring.h"
#include "lights.h"
#include "sessions.h"
#include "server.h"
#include "ui.h"

// Periodic Serial output is not free on this board. There is no UART bridge,
// so Serial is USB CDC, and a write blocks until the host drains it - or until
// its timeout expires when nothing is listening. A print every five seconds
// therefore shows up as the display running smoothly for a few seconds, then
// pausing, then running again.
//
// That is a trap worth naming: the instrumentation added to find a stutter was
// itself producing one. Both profiling prints are off by default; turn them on
// only while attached to a serial monitor that is actually reading.
#ifndef UI_PROFILE
#define UI_PROFILE 0
#endif

// Screen orientation, in degrees clockwise: 0, 90, 180 or 270.
//
// Done in software - see the note where the display driver is registered. Both
// the rendering and the touch mapping key off this one value; change it here
// and they stay in agreement.
// 90 for this enclosure, established by looking at it rather than by deriving
// it: the panel's scan order and the gate's mounting both have a say, and the
// two together are not worth reasoning about from first principles.
#ifndef SCREEN_ROTATION
#define SCREEN_ROTATION 90
#endif

// --- display ----------------------------------------------------------------

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

// The trailing offsets are the panel's own window origin; they come from
// Waveshare's examples for this display and are not tunable guesses.
static Arduino_CO5300 *gfx =
    new Arduino_CO5300(bus, LCD_RESET, 0 /* rotation */, LCD_WIDTH, LCD_HEIGHT,
                       6, 0, 0, 0);

static TouchDrvCST92xx touch;
static volatile bool touchIRQ = false;

static lv_disp_draw_buf_t drawBuf;

// --- application ------------------------------------------------------------

static Registry registry(SESSION_TTL_MS, WORKING_TTL_MS, DONE_LINGER_MS,
                         BACKGROUND_HOLD_MS);
static Ring ring(RING_PIN, LED_COUNT, RING_BRIGHTNESS, THEME);
static HookServer server(registry, HTTP_PORT, HOOK_TOKEN);

// One animation clock for both surfaces. Owned by the ring task, read by the
// UI: two independently ticking phases would drift and the arcs would stop
// agreeing about where a comet is.
static volatile uint32_t g_phase = 0;

// --- LVGL glue --------------------------------------------------------------

// Time spent inside flush, accumulated per reporting window. Separating this
// from the total tells render cost from transfer cost, which are fixed in
// completely different places.
static volatile uint32_t g_flushUs = 0;
static volatile uint32_t g_flushPx = 0;

#if SCREEN_ROTATION != 0
// Scratch for the transpose. One flush area's worth, allocated once.
static lv_color_t *rotBuf = nullptr;
#endif

static void lvglFlush(lv_disp_drv_t *disp, const lv_area_t *area,
                      lv_color_t *pixels) {
  uint32_t t0 = micros();
  int32_t x1 = area->x1, y1 = area->y1, x2 = area->x2, y2 = area->y2;
  uint32_t w = x2 - x1 + 1;
  uint32_t h = y2 - y1 + 1;
  lv_color_t *out = pixels;
  int32_t ox = x1, oy = y1;
  uint32_t ow = w, oh = h;

#if SCREEN_ROTATION != 0
  // Rotate here rather than with LVGL's sw_rotate.
  //
  // LVGL rotates in chunks of `LV_DISP_ROT_MAX_BUF / area_width` rows and
  // gives each chunk its own flush window, at whatever parity the arithmetic
  // lands on. The CO5300 addresses pixels in PAIRS - which is the entire
  // reason lvglRounder() exists - so every chunk that starts on an odd column
  // is written one pixel out. On the status page, with its many narrow arcs,
  // that is a screenful of smeared garbage; on the lights page, with a few
  // wide areas, it mostly happens to line up.
  //
  // Rotating the whole area in one go keeps the rounder's guarantee: it makes
  // both axes even-start and odd-end, and every mapping below carries that
  // through to the panel.
  const int32_t SIDE = LCD_WIDTH;  // square panel, so no resolution swap
  if (rotBuf) {
    out = rotBuf;
#if SCREEN_ROTATION == 90
    ox = SIDE - 1 - y2;
    oy = x1;
    ow = h;
    oh = w;
    for (uint32_t sy = 0; sy < h; sy++)
      for (uint32_t sx = 0; sx < w; sx++)
        out[sx * h + (h - 1 - sy)] = pixels[sy * w + sx];
#elif SCREEN_ROTATION == 180
    ox = SIDE - 1 - x2;
    oy = SIDE - 1 - y2;
    for (uint32_t sy = 0; sy < h; sy++)
      for (uint32_t sx = 0; sx < w; sx++)
        out[(h - 1 - sy) * w + (w - 1 - sx)] = pixels[sy * w + sx];
#else  // 270
    ox = y1;
    oy = SIDE - 1 - x2;
    ow = h;
    oh = w;
    for (uint32_t sy = 0; sy < h; sy++)
      for (uint32_t sx = 0; sx < w; sx++)
        out[(w - 1 - sx) * h + sy] = pixels[sy * w + sx];
#endif
  }
#endif

#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(ox, oy, (uint16_t *)&out->full, ow, oh);
#else
  gfx->draw16bitRGBBitmap(ox, oy, (uint16_t *)&out->full, ow, oh);
#endif
  g_flushUs += micros() - t0;
  g_flushPx += w * h;
  lv_disp_flush_ready(disp);
}

// The CO5300 addresses pixels in pairs, so every flush window has to start on
// an even coordinate and end on an odd one. Without this, partial redraws tear.
static void lvglRounder(lv_disp_drv_t *disp, lv_area_t *area) {
  (void)disp;
  if (area->x1 % 2 != 0) area->x1--;
  if (area->y1 % 2 != 0) area->y1--;
  if (area->x2 % 2 == 0) area->x2++;
  if (area->y2 % 2 == 0) area->y2++;
}

static void IRAM_ATTR onTouchIRQ() { touchIRQ = true; }

static void lvglTouchRead(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  (void)drv;
  static int16_t lastX = 0, lastY = 0;

  // Touch state from BOTH signals, with a hold-off.
  //
  // Neither signal alone is trustworthy on this part:
  //   - getPoint() only returns coordinates around a report, so a still finger
  //     produces bursts with quiet gaps between them
  //   - the INT pin PULSES per report rather than being held low for the
  //     duration of a touch, so isPressed() flaps during a single press
  //
  // Taking either as authoritative gives multiple press/release pairs for one
  // physical tap - which showed up as a single tap on the power button being
  // counted as a double tap and flipping the page.
  //
  // So: any evidence of contact refreshes a deadline, and the touch is
  // considered down until that deadline passes. 150 ms is longer than the gap
  // between reports and far shorter than a deliberate lift.
  static uint32_t contactUntil = 0;
  int16_t xs[5], ys[5];
  bool evidence = touch.isPressed();

  if (evidence || touchIRQ) {
    touchIRQ = false;
    if (touch.getPoint(xs, ys, touch.getSupportTouchPoint()) > 0) {
      lastX = xs[0];
      lastY = ys[0];  // panel reports Y correctly; see note above
      evidence = true;
    }
  }
  if (evidence) contactUntil = millis() + 150;
  bool pressed = (int32_t)(contactUntil - millis()) > 0;

  // Swipe, as a shortcut alongside the nav pills.
  //
  // Computed from where a press started and ended rather than from LVGL's
  // gesture engine, which infers motion between samples and needs a
  // continuous stream this panel does not provide. Start-versus-end does not
  // care about sample rate. The pills remain the reliable route; this is a
  // convenience on top, and it is only trustworthy now that coordinates are
  // no longer being mirrored out from under it.
  static bool prevPressed = false;
  static int16_t startX = 0, startY = 0;
  static uint32_t startMs = 0;

  if (pressed && !prevPressed) {
    startX = lastX;
    startY = lastY;
    startMs = millis();
  } else if (!pressed && prevPressed) {
    int dx = (int)lastX - startX;
    int dy = (int)lastY - startY;
    // Either direction toggles: with two pages, direction carries no meaning.
    if (abs(dx) > 70 && abs(dx) > abs(dy) * 2 && millis() - startMs < 1200) {
      uiTogglePage();
    }
  }
  prevPressed = pressed;

  static bool wasDown = false;
#if TOUCH_DEBUG
  if (pressed && !wasDown) Serial.printf("touch %d,%d\n", lastX, lastY);
#endif
  wasDown = pressed;

  // Panel coordinates into rotated screen coordinates. LVGL rotates what it
  // draws; it does not rotate what the touch driver reports, so this has to
  // match by hand.
  //
  // The panel is square, so no resolution swap is needed. Rotating the image
  // 90 degrees clockwise puts the logical origin at the panel's top-right:
  // logical x runs down the panel, logical y runs back along it.
  int16_t tx = lastX, ty = lastY;
#if SCREEN_ROTATION == 90
  data->point.x = ty;
  data->point.y = (LCD_WIDTH - 1) - tx;
#elif SCREEN_ROTATION == 180
  data->point.x = (LCD_WIDTH - 1) - tx;
  data->point.y = (LCD_HEIGHT - 1) - ty;
#elif SCREEN_ROTATION == 270
  data->point.x = (LCD_HEIGHT - 1) - ty;
  data->point.y = tx;
#else
  data->point.x = tx;
  data->point.y = ty;
#endif
  data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void lvglTick(void *arg) {
  (void)arg;
  lv_tick_inc(2);
}

// --- ring task --------------------------------------------------------------

// The ring runs on its own task so a long LVGL redraw cannot stutter it. It
// takes a snapshot rather than holding the registry lock for a whole frame.
static void ringTask(void *arg) {
  (void)arg;
  // Static, not stack: a Session now carries the full path and a line of
  // prompt, so 16 of them is ~4.3 KB - enough to matter against an 8 KB task
  // stack. Safe because only this task touches it.
  static Session snap[MAX_SESSIONS];
  uint32_t phase = 0;
  uint32_t frames = 0;
  uint32_t lastReport = 0;

  for (;;) {
    if ((int32_t)(g_ringTestUntil - millis()) > 0) {
      if (g_ringOverride == RING_OVERRIDE_ZERO) {
        ring.zeroMarker();
      } else if (g_ringOverride == RING_OVERRIDE_SCAN) {
        ring.pinScan(phase);
      } else if (g_ringOverride == RING_OVERRIDE_HIGH) {
        ring.holdPinHigh(g_ringHighPin);
      } else {
        ring.testPattern(phase);
      }
    } else {
      size_t n = registry.snapshot(snap, MAX_SESSIONS);
      ring.render(snap, n, phase, DONE_LINGER_MS);
    }
    phase = (phase + 1) & 0x7FFF;
    g_phase = phase;
    frames++;

    // A dark ring and a dead task look identical from outside, which cost an
    // hour of chasing wiring that was fine. This makes the difference audible:
    // no heartbeat means the task died, a heartbeat with a dark ring means the
    // pixels are being written and something past show() is wrong.
#if UI_PROFILE
    if (millis() - lastReport >= 5000) {
      lastReport = millis();
      Serial.printf("ring: %lu frames, stack headroom %u bytes\n",
                    (unsigned long)frames,
                    (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
#else
    (void)lastReport;
#endif

    vTaskDelay(pdMS_TO_TICKS(30));  // ~33 fps
  }
}

// --- network ----------------------------------------------------------------

static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
#ifdef USE_STATIC_IP
  IPAddress ip(STATIC_IP), gw(STATIC_GATEWAY), sn(STATIC_SUBNET),
      dns(STATIC_DNS);
  if (!WiFi.config(ip, gw, sn, dns)) {
    Serial.println("wifi: static config rejected, falling back to DHCP");
  }
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("wifi: connecting");
  uint32_t deadline = millis() + 20000;
  while (WiFi.status() != WL_CONNECTED && (int32_t)(millis() - deadline) < 0) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("console: listening on http://%s:%d/hook\n",
                  WiFi.localIP().toString().c_str(), HTTP_PORT);
  } else {
    // Not fatal: the ring and screen still work, and the watchdog below keeps
    // retrying. A status light should not brick itself over a slow router.
    Serial.println("wifi: not connected yet, will keep retrying");
  }
}

static void publishAddress() {
  static String shown;
  String now = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString()
                                             : String("no wifi");
  if (now != shown) {
    shown = now;
    uiSetNetwork(now.c_str());
  }
}

// --- setup / loop -----------------------------------------------------------

void setup() {
  Serial.begin(115200);
  // Drop output when no host is reading rather than blocking on it. Without
  // this, a CDC write stalls until its timeout whenever no monitor is attached,
  // which is what turned a 5 s profiling print into a visible stutter and a
  // per-press log into a swipe that would not register.
  Serial.setTxTimeoutMs(0);
  delay(200);
  Serial.println("\nclaude-status-console");

  Wire.begin(IIC_SDA, IIC_SCL);

  // The ring comes up FIRST, before LVGL's draw buffers are allocated.
  //
  // Adafruit_NeoPixel drives WS2812Bs from the RMT peripheral, and rmtInit()
  // needs internal DMA-capable RAM. Two 43 KB LVGL buffers take most of what
  // is available, and if rmtInit() then fails, espShow() returns while still
  // holding its mutex - so every subsequent show() blocks 50 ms and silently
  // does nothing, forever. No crash, no log at the default debug level, just
  // a permanently dark ring on a board where everything else works.
  //
  // Claiming the RMT channel here, while memory is uncontended, avoids the
  // whole failure mode. Order matters; do not move this below lv_init().
  if (!ring.begin()) Serial.println("ring: begin() failed, ring disabled");
  BaseType_t ringOk =
      xTaskCreatePinnedToCore(ringTask, "ring", 8192, nullptr, 2, nullptr, 1);
  Serial.printf("ring: task %s\n",
                ringOk == pdPASS ? "started" : "FAILED TO START");

  touch.setPins(TP_RESET, TP_INT);
  if (!touch.begin(Wire, TP_ADDR, IIC_SDA, IIC_SCL)) {
    // Touch is a convenience here, not a dependency - the arcs and the ring
    // carry the status on their own, so a dead touch panel must not stop boot.
    Serial.println("touch: not found, continuing without it");
  } else {
    Serial.printf("touch: %s\n", touch.getModelName());
    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    // X mirrored, Y NOT. Waveshare's example uses (true, true); measured on
    // this panel, that flips the bottom of the screen to the top.
    //
    // Verified by pressing three known spots and logging what arrived:
    //   centre       -> 233,233   (invariant under mirroring - hid the bug)
    //   bottom-left  -> 119,76    should have been 119,390
    //   bottom-right -> 347,76    should have been 347,390
    //
    // 466 - 390 = 76. Every press on the bottom row was landing near the top,
    // which is why the power button in the centre worked from the first flash
    // while nothing in the button row ever did - and why three rewrites of the
    // touch debounce chased a problem that was never about timing.
    touch.setMirrorXY(true, true);  // no-op on this driver; see the flip above
    attachInterrupt(TP_INT, onTouchIRQ, FALLING);
  }

  // 80 MHz rather than the library's 40 MHz default. At 40 MHz a full 466x466
  // frame is ~22 ms of bus time alone, which caps a full-screen redraw below
  // 30 fps before LVGL has rendered anything - and the event horizon redraws
  // the whole centre disc every frame. The CO5300 is specified well past this.
  if (!gfx->begin(80000000)) Serial.println("gfx: begin() failed");
  gfx->fillScreen(RGB565_BLACK);
  gfx->setBrightness(SCREEN_BRIGHTNESS);

  lv_init();

  // A tenth of the screen per buffer, in internal DMA-capable RAM. The vendor
  // examples use a quarter, but they are not also running WiFi and an HTTP
  // server; two quarter-screen buffers would take ~217 KB of the 512 KB and
  // leave the network stack short.
  // A sixteenth of the screen per buffer, not a tenth, because rotation needs
  // a third buffer of the same size for the transpose.
  //
  //   2 x 1/10 = 87 KB   (no rotation, the long-standing baseline)
  //   3 x 1/10 = 130 KB  (rotation - and WiFi then fails to init with
  //                       ESP_ERR_NO_MEM, because this is DMA-capable
  //                       internal RAM and the radio needs its share)
  //   3 x 1/16 = 81 KB   (rotation, and still under the baseline)
  //
  // Smaller buffers mean more flush calls, each one cheaper. The total is what
  // the radio cares about.
  size_t bufPx = (size_t)LCD_WIDTH * LCD_HEIGHT / 16;
  lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(
      bufPx * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(
      bufPx * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!buf1 || !buf2) {
    Serial.println("lvgl: draw buffer allocation failed");
  }
  lv_disp_draw_buf_init(&drawBuf, buf1, buf2, bufPx);

  static lv_disp_drv_t dispDrv;
  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = LCD_WIDTH;
  dispDrv.ver_res = LCD_HEIGHT;
  dispDrv.flush_cb = lvglFlush;
  dispDrv.rounder_cb = lvglRounder;
  dispDrv.draw_buf = &drawBuf;
  // Software rotation, because the panel cannot do it. Arduino_CO5300's
  // setRotation only emits MADCTL X/Y flips - no row/column exchange - so
  // rotation 1 mirrors the image rather than turning it. LVGL transposes each
  // flushed area into a scratch buffer instead: correct for text and layout
  // alike, at the cost of a copy per flush.
  //
  // Touch is rotated to match in lvglTouchRead(). Both read SCREEN_ROTATION,
  // so they cannot drift apart - a display and a touch layer disagreeing about
  // which way is up is precisely the bug that cost a day here before.
#if SCREEN_ROTATION != 0
  // Not dispDrv.sw_rotate - see the note in lvglFlush(). LVGL's chunked
  // rotation breaks the panel's even-column requirement; ours does not.
  rotBuf = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t),
                                          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!rotBuf) {
    Serial.println("rotate: scratch buffer failed - display will be UNROTATED");
  }
#endif
  lv_disp_drv_register(&dispDrv);

  static lv_indev_drv_t indevDrv;
  lv_indev_drv_init(&indevDrv);
  indevDrv.type = LV_INDEV_TYPE_POINTER;
  indevDrv.read_cb = lvglTouchRead;
  // Easier swipes than the defaults (50 px, velocity 3). This panel reports
  // coordinates in bursts around interrupts rather than continuously, so a
  // deliberate drag can arrive as only a few samples - not enough travel or
  // velocity to clear the stock thresholds.
  indevDrv.gesture_limit = 30;
  indevDrv.gesture_min_velocity = 2;
  lv_indev_drv_register(&indevDrv);

  const esp_timer_create_args_t tickArgs = {.callback = &lvglTick,
                                            .name = "lvgl_tick"};
  esp_timer_handle_t tickTimer = nullptr;
  esp_timer_create(&tickArgs, &tickTimer);
  esp_timer_start_periodic(tickTimer, 2 * 1000);

  uiInit(resolvePalette(THEME), LED_COUNT);

  connectWiFi();
  publishAddress();
  // NTP before the light client: Tuya control payloads carry a timestamp.
  configTime(0, 0, "pool.ntp.org");
  lightsBegin(LIGHT_IP, LIGHT_ID, LIGHT_KEY);

  server.begin();

  // Draw every page once before anyone touches one. A page that only renders
  // on demand is a page only a finger can test.
  uiSelfTest();

  Serial.println("setup done");
}

void loop() {
  static uint32_t lastPrune = 0, lastUi = 0, lastWiFi = 0;
  uint32_t now = millis();

  server.poll();
  lightsPoll();

  if (now - lastPrune >= 3000) {
    lastPrune = now;
    registry.prune();
  }

  if (now - lastWiFi >= 30000) {
    lastWiFi = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("wifi: reconnecting");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    publishAddress();
  }

  if (now - lastUi >= 33) {
    lastUi = now;
    static Session snap[MAX_SESSIONS];  // see the note in ringTask
    size_t n = registry.snapshot(snap, MAX_SESSIONS);
    uiUpdate(snap, n, g_phase, DONE_LINGER_MS);
  }

  // Frame-time instrumentation. A stuttering animation and an overloaded bus
  // look identical from the sofa, so measure rather than guess - the same
  // reason ringTask reports its frame count.
  uint32_t t0 = micros();
  lv_timer_handler();
  uint32_t dt = micros() - t0;

#if UI_PROFILE
  static uint32_t uiFrames = 0, uiSum = 0, uiMax = 0, uiLast = 0;
  uiFrames++;
  uiSum += dt;
  if (dt > uiMax) uiMax = dt;
  if (now - uiLast >= 5000) {
    uiLast = now;
    Serial.printf(
        "ui: %lu passes/5s, mean %lu us, worst %lu us | flush %lu us "
        "(%lu%%), %lu kpx\n",
        (unsigned long)uiFrames,
        (unsigned long)(uiFrames ? uiSum / uiFrames : 0), (unsigned long)uiMax,
        (unsigned long)(uiFrames ? g_flushUs / uiFrames : 0),
        (unsigned long)(uiSum ? (uint64_t)g_flushUs * 100 / uiSum : 0),
        (unsigned long)(g_flushPx / 1000));
    uiFrames = uiSum = uiMax = 0;
    g_flushUs = g_flushPx = 0;
  }
#else
  (void)dt;
#endif

  delay(2);
}
