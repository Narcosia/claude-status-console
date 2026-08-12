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
#include "sessions.h"
#include "server.h"
#include "ui.h"

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

static void lvglFlush(lv_disp_drv_t *disp, const lv_area_t *area,
                      lv_color_t *pixels) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&pixels->full, w, h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&pixels->full, w, h);
#endif
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
  static bool held = false;

  // Poll on the interrupt, and keep polling while a finger is down so the
  // release is seen. An edge-triggered flag alone can leave LVGL believing a
  // press is still held.
  if (touchIRQ || held) {
    touchIRQ = false;
    int16_t xs[5], ys[5];
    uint8_t n = touch.getPoint(xs, ys, touch.getSupportTouchPoint());
    if (n > 0) {
      lastX = xs[0];
      lastY = ys[0];
      held = true;
    } else {
      held = false;
    }
  }

  data->point.x = lastX;
  data->point.y = lastY;
  data->state = held ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
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
    if (millis() - lastReport >= 5000) {
      lastReport = millis();
      Serial.printf("ring: %lu frames, stack headroom %u bytes\n",
                    (unsigned long)frames,
                    (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }

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
    touch.setMirrorXY(true, true);
    attachInterrupt(TP_INT, onTouchIRQ, FALLING);
  }

  if (!gfx->begin()) Serial.println("gfx: begin() failed");
  gfx->fillScreen(RGB565_BLACK);
  gfx->setBrightness(SCREEN_BRIGHTNESS);

  lv_init();

  // A tenth of the screen per buffer, in internal DMA-capable RAM. The vendor
  // examples use a quarter, but they are not also running WiFi and an HTTP
  // server; two quarter-screen buffers would take ~217 KB of the 512 KB and
  // leave the network stack short.
  size_t bufPx = (size_t)LCD_WIDTH * LCD_HEIGHT / 10;
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
  lv_disp_drv_register(&dispDrv);

  static lv_indev_drv_t indevDrv;
  lv_indev_drv_init(&indevDrv);
  indevDrv.type = LV_INDEV_TYPE_POINTER;
  indevDrv.read_cb = lvglTouchRead;
  lv_indev_drv_register(&indevDrv);

  const esp_timer_create_args_t tickArgs = {.callback = &lvglTick,
                                            .name = "lvgl_tick"};
  esp_timer_handle_t tickTimer = nullptr;
  esp_timer_create(&tickArgs, &tickTimer);
  esp_timer_start_periodic(tickTimer, 2 * 1000);

  uiInit(resolvePalette(THEME), LED_COUNT);

  connectWiFi();
  publishAddress();
  server.begin();

  Serial.println("setup done");
}

void loop() {
  static uint32_t lastPrune = 0, lastUi = 0, lastWiFi = 0;
  uint32_t now = millis();

  server.poll();

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

  lv_timer_handler();
  delay(2);
}
