#include "photo_capture.h"
#include <Arduino.h>
#include <WiFi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_camera.h"
#include "esp32-hal-ledc.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "board_config.h"
#include "sd_storage.h"
#include "device_log.h"
#include "water_level.h"

static SemaphoreHandle_t cameraBusy = NULL;
static SemaphoreHandle_t latestMutex = NULL;
static volatile bool streaming = false;
static volatile bool flashEnabled = true;
static volatile bool torchOn = false;  // steady live-view light, independent of capture flash
static volatile bool cameraOk = true;  // false if esp_camera_init() failed at boot
static framesize_t currentFramesize = FRAMESIZE_UXGA;
static framesize_t maxFramesize = FRAMESIZE_UXGA;      // full-res capture target; set from setup() per detected sensor
static framesize_t streamFramesize = FRAMESIZE_SVGA;   // live-view target; sensor-aware, set from setup()
static int currentQuality = -1;   // JPEG quality actually set on the sensor right now (-1 = unknown, forces first set)
static int streamQuality = 12;    // higher number = more compression = smaller/faster frames
static int captureQuality = 10;
static uint32_t streamMinIntervalMs = 0;

static uint8_t *latestBuf = NULL;
static size_t latestLen = 0;
static size_t latestCapacity = 0;

// Snap-during-stream handshake: the HTTP snap handler raises a request,
// the stream loop services it with the frame it just grabbed.
static volatile bool snapRequested = false;
static volatile bool snapReqOk = false;
static char snapReqFilename[48] = { 0 };
static SemaphoreHandle_t snapDone = NULL;

void photoCaptureBegin() {
  cameraBusy = xSemaphoreCreateMutex();
  latestMutex = xSemaphoreCreateMutex();
  snapDone = xSemaphoreCreateBinary();
#if defined(LED_GPIO_NUM)
  // On AI-Thinker boards the flash LED and the SD card's DATA1 line both
  // sit on GPIO4. SD_MMC.begin() (called before this, in setup()) leaves
  // that pin configured for the SD peripheral even in 1-line mode; without
  // reclaiming it here, ledcAttach() silently has no effect and the flash
  // never fires. gpio_reset_pin() detaches it from the SD/RTC/matrix
  // function first so LEDC can drive it as a plain PWM output.
  gpio_reset_pin((gpio_num_t)LED_GPIO_NUM);
  ledcAttach(LED_GPIO_NUM, 5000, 8);
  ledcWrite(LED_GPIO_NUM, 0);
#endif
}

bool photoCaptureIsStreaming() {
  return streaming;
}

void photoCaptureSetCameraOk(bool ok) {
  cameraOk = ok;
}

bool photoCaptureCameraOk() {
  return cameraOk;
}

void photoCaptureSetMaxFramesize(framesize_t fs) {
  maxFramesize = fs;
}

void photoCaptureSetStreamFramesize(framesize_t fs) {
  streamFramesize = fs;
}

// Call once after camera_setup so internal state matches the sensor's idle
// (stream) resolution set during boot.
void photoCaptureSyncIdleMode() {
  currentFramesize = streamFramesize;
  currentQuality = streamQuality;
}

void photoCaptureSetQuality(int newStreamQuality, int newCaptureQuality) {
  streamQuality = newStreamQuality;
  captureQuality = newCaptureQuality;
}

void photoCaptureSetStreamMinIntervalMs(uint32_t ms) {
  streamMinIntervalMs = ms;
}

uint32_t photoCaptureStreamMinIntervalMs() {
  return streamMinIntervalMs;
}

void photoCaptureSetFlashEnabled(bool enabled) {
  flashEnabled = enabled;
#if defined(LED_GPIO_NUM)
  if (!enabled) {
    ledcWrite(LED_GPIO_NUM, torchOn ? 255 : 0);  // in case it was mid-flash when disabled
  }
#endif
}

bool photoCaptureFlashEnabled() {
  return flashEnabled;
}

// Steady live-view light -- lit continuously until toggled off, unrelated
// to the brief pulse used during a capture. Captures still flash normally
// on top of it; the LED is restored to the torch state afterwards instead
// of being forced off (see grabFrame()).
void photoCaptureSetTorch(bool on) {
  torchOn = on;
#if defined(LED_GPIO_NUM)
  ledcWrite(LED_GPIO_NUM, on ? 255 : 0);
#endif
}

bool photoCaptureTorchOn() {
  return torchOn;
}

bool photoCaptureTryBeginStream() {
  if (!cameraOk) {
    return false;
  }
  if (xSemaphoreTake(cameraBusy, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
  sensor_t *s = esp_camera_sensor_get();
  if (currentFramesize != streamFramesize) {
    s->set_framesize(s, streamFramesize);
    currentFramesize = streamFramesize;
  }
  if (currentQuality != streamQuality) {
    s->set_quality(s, streamQuality);
    currentQuality = streamQuality;
  }
  streaming = true;
  xSemaphoreGive(cameraBusy);
  return true;
}

void photoCaptureEndStream() {
  if (xSemaphoreTake(cameraBusy, pdMS_TO_TICKS(1000)) == pdTRUE) {
    streaming = false;
    xSemaphoreGive(cameraBusy);
  }
}

static void updateLatest(const uint8_t *data, size_t len) {
  if (xSemaphoreTake(latestMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return;
  }
  if (len > latestCapacity) {
    uint8_t *bigger = (uint8_t *)realloc(latestBuf, len);
    if (!bigger) {
      xSemaphoreGive(latestMutex);
      return;
    }
    latestBuf = bigger;
    latestCapacity = len;
  }
  memcpy(latestBuf, data, len);
  latestLen = len;
  xSemaphoreGive(latestMutex);
}

bool photoCaptureLockLatest(const uint8_t **outBuf, size_t *outLen) {
  if (xSemaphoreTake(latestMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
  if (!latestBuf || latestLen == 0) {
    xSemaphoreGive(latestMutex);
    return false;
  }
  *outBuf = latestBuf;
  *outLen = latestLen;
  return true;
}

void photoCaptureUnlockLatest() {
  xSemaphoreGive(latestMutex);
}

// outFilename from sdStorageSavePhoto() is "<date>/<name>" -- split that
// back apart to hand off to the AI vision queue (a no-op if vision is off).
static void enqueueVisionIfAny(const char *dateSlashName) {
  const char *slash = strchr(dateSlashName, '/');
  if (!slash) {
    return;
  }
  char date[16];
  size_t dateLen = (size_t)(slash - dateSlashName);
  if (dateLen >= sizeof(date)) {
    return;
  }
  memcpy(date, dateSlashName, dateLen);
  date[dateLen] = '\0';
  waterLevelEnqueueAnalysis(date, slash + 1);
}

static void discardOneFrame() {
  camera_fb_t *stale = esp_camera_fb_get();
  if (stale) {
    esp_camera_fb_return(stale);
  }
}

// After a full-res capture, drop back to the low-res stream profile and
// flush any high-res frames still sitting in the driver queue.
static void restoreStreamMode(sensor_t *s) {
  if (currentFramesize != streamFramesize) {
    s->set_framesize(s, streamFramesize);
    currentFramesize = streamFramesize;
  }
  if (currentQuality != streamQuality) {
    s->set_quality(s, streamQuality);
    currentQuality = streamQuality;
  }
  discardOneFrame();
}

// Grabs one frame, firing the flash first if enabled.
//
// `justResized` should be true when the sensor's framesize was changed
// immediately before this call. The sensor needs time to re-settle
// exposure/gain (AEC/AGC) after a resolution switch -- without a flush
// here, the first frame back is often soft or wrongly exposed regardless
// of flash, which is the main source of blurry/dark captures.
//
// A high-megapixel sensor (OV5640 at ~5MP vs OV2640's ~2MP) has a much
// longer frame period, so each "discard a stale frame" step costs a lot
// more real time there. maxFramesize is used as a proxy for "which sensor
// is this": settling/discard work is trimmed down for big sensors, both
// because their own explicit waits already span most of a frame period
// (so extra discards buy little) and because capture latency is the thing
// actually being complained about at 5MP.
static camera_fb_t *grabFrame(bool justResized) {
  bool highRes = maxFramesize > FRAMESIZE_UXGA;

  // Skip the standalone post-resize settle when flash is about to run on
  // a high-res sensor -- the flash sequence below does its own wait+
  // discard, which already covers the same settling purpose, so paying
  // for both back to back would just be wasted time on a slow sensor.
  if (justResized && !(highRes && flashEnabled)) {
    vTaskDelay(pdMS_TO_TICKS(100));
    discardOneFrame();
  }

#if defined(LED_GPIO_NUM)
  // Flash + camera sensor + a WiFi transmit burst (e.g. mid-stream, or
  // the HTTP response for this very request) drawing current at the same
  // moment is the classic ESP32-CAM brownout trigger on a marginal 5V
  // supply. Trimming TX power for the ~0.5s the flash is on costs a
  // negligible amount of range/throughput and meaningfully cuts the peak.
  wifi_power_t prevTxPower = WIFI_POWER_19_5dBm;
  if (flashEnabled) {
    prevTxPower = WiFi.getTxPower();
    WiFi.setTxPower(WIFI_POWER_11dBm);

    ledcWrite(LED_GPIO_NUM, 255);
    vTaskDelay(pdMS_TO_TICKS(200));  // let the LED reach full brightness

    // Flush frames that may have started exposing before the flash was
    // at full brightness. On a fast sensor (OV2640) the driver's double
    // buffering means one frame can already be mid-capture and another
    // queued by the time the flash turns on, so a single discard still
    // risks handing back a partially-lit frame -- discard two there. A
    // high-res sensor's frame period is long enough that the 200ms wait
    // above already spans close to a full frame, so one discard suffices
    // and a second would just add latency for no real benefit.
    discardOneFrame();
    if (!highRes) {
      discardOneFrame();
    }
  }
#endif

  camera_fb_t *fb = esp_camera_fb_get();

#if defined(LED_GPIO_NUM)
  if (flashEnabled) {
    ledcWrite(LED_GPIO_NUM, torchOn ? 255 : 0);  // restore torch state, don't just force off
    WiFi.setTxPower(prevTxPower);
  }
#endif

  return fb;
}

// Grabs at maxFramesize; if that fails (NULL fb, or a non-JPEG/truncated
// frame) falls back once to FRAMESIZE_UXGA before giving up outright.
// Very high resolutions (QSXGA on an OV5640 especially) are a known weak
// point on many modules/wiring -- unreliable JPEG capture there even when
// everything else about the setup is fine -- so a hard failure at
// maxFramesize is treated as "try smaller" rather than "capture failed".
// *outJustResized reports whether the sensor's framesize actually changed
// this call (used by the caller only to decide on grabFrame's settle step,
// so it reflects the LAST framesize switch, not necessarily maxFramesize).
static camera_fb_t *captureFullResWithFallback(sensor_t *s, bool *outJustResized) {
  bool resized = false;
  if (currentFramesize != maxFramesize) {
    s->set_framesize(s, maxFramesize);
    currentFramesize = maxFramesize;
    resized = true;
  }
  if (currentQuality != captureQuality) {
    s->set_quality(s, captureQuality);
    currentQuality = captureQuality;
  }

  camera_fb_t *fb = grabFrame(resized);
  if (fb && fb->format == PIXFORMAT_JPEG && fb->len > 0) {
    if (outJustResized) *outJustResized = resized;
    return fb;
  }
  if (fb) {
    esp_camera_fb_return(fb);
  }
  if (maxFramesize <= FRAMESIZE_UXGA) {
    if (outJustResized) *outJustResized = resized;
    return NULL;  // already at the fallback size; nothing smaller to retry with
  }

  s->set_framesize(s, FRAMESIZE_UXGA);
  currentFramesize = FRAMESIZE_UXGA;
  fb = grabFrame(true);
  if (outJustResized) *outJustResized = true;
  return fb;  // may still be NULL; caller treats that as a genuine failure
}

// While streaming we cannot take over the camera from the HTTP request
// thread, so raise a request and let the stream loop service it (see
// photoCaptureServiceStreamSnap) between two of its own frame grabs.
static bool snapViaStream(char *outFilename, size_t outFilenameSize) {
  xSemaphoreTake(snapDone, 0);  // clear any stale completion
  snapReqOk = false;
  snapRequested = true;
  // Generous timeout: the stream loop does two framesize switches (stream
  // size -> full res -> stream size) plus a full-resolution grab and
  // possibly a flash pulse before it can signal completion. A 5MP sensor
  // can genuinely take several seconds for that whole sequence -- the
  // previous 6s budget was tight enough to look like "capture failed" on
  // an OV5640 even when nothing was actually wrong.
  if (xSemaphoreTake(snapDone, pdMS_TO_TICKS(15000)) != pdTRUE) {
    snapRequested = false;  // stream ended or stalled; give up
    return false;
  }
  if (snapReqOk && outFilename && outFilenameSize > 0) {
    snprintf(outFilename, outFilenameSize, "%s", snapReqFilename);
  }
  return snapReqOk;
}

// Called by the stream loop once per frame, before it grabs its own
// (low-res) frame. If a full-resolution snap was requested, this briefly
// switches the sensor to max capture resolution, grabs and saves one frame
// to SD, then switches back to stream resolution so live view resumes.
void photoCaptureServiceStreamSnap() {
  if (!snapRequested) {
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  camera_fb_t *fb = captureFullResWithFallback(s, NULL);
  if (fb) {
    snapReqOk = sdStorageSavePhoto(fb->buf, fb->len, snapReqFilename, sizeof(snapReqFilename));
    if (snapReqOk) {
      updateLatest(fb->buf, fb->len);
      enqueueVisionIfAny(snapReqFilename);
    }
  } else {
    snapReqOk = false;
  }
  if (fb) {
    esp_camera_fb_return(fb);
  }

  restoreStreamMode(s);

  snapRequested = false;
  xSemaphoreGive(snapDone);
}

bool photoCaptureSnap(char *outFilename, size_t outFilenameSize) {
  if (!cameraOk) {
    return false;
  }
  if (streaming) {
    return snapViaStream(outFilename, outFilenameSize);
  }
  if (xSemaphoreTake(cameraBusy, pdMS_TO_TICKS(2000)) != pdTRUE) {
    return false;
  }
  if (streaming) {
    xSemaphoreGive(cameraBusy);
    return snapViaStream(outFilename, outFilenameSize);
  }

  sensor_t *s = esp_camera_sensor_get();
  camera_fb_t *fb = captureFullResWithFallback(s, NULL);
  if (!fb) {
    xSemaphoreGive(cameraBusy);
    return false;
  }

  bool saved = sdStorageSavePhoto(fb->buf, fb->len, outFilename, outFilenameSize);
  if (saved) {
    updateLatest(fb->buf, fb->len);
    enqueueVisionIfAny(outFilename);
  }

  esp_camera_fb_return(fb);

  // Return to low-res stream mode even when live view is off, so the next
  // stream session or timed capture starts from the fast profile.
  restoreStreamMode(s);

  xSemaphoreGive(cameraBusy);
  return saved;
}

bool photoCaptureSnapToBuffer(framesize_t size, uint8_t **outBuf, size_t *outLen) {
  if (!cameraOk) {
    deviceLog("[capture] snapToBuffer failed: camera not initialized (cameraOk=false)");
    return false;
  }
  if (streaming) {
    deviceLog("[capture] snapToBuffer failed: live view is streaming");
    return false;
  }
  if (xSemaphoreTake(cameraBusy, pdMS_TO_TICKS(2000)) != pdTRUE) {
    deviceLog("[capture] snapToBuffer failed: camera busy (mutex held >2s by another capture)");
    return false;
  }
  if (streaming) {
    xSemaphoreGive(cameraBusy);
    deviceLog("[capture] snapToBuffer failed: streaming started while waiting for the camera");
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  bool resized = false;
  if (currentFramesize != size) {
    s->set_framesize(s, size);
    currentFramesize = size;
    resized = true;
  }
  if (currentQuality != captureQuality) {
    s->set_quality(s, captureQuality);
    currentQuality = captureQuality;
  }

  camera_fb_t *fb = grabFrame(resized);
  if (!fb) {
    xSemaphoreGive(cameraBusy);
    deviceLog("[capture] snapToBuffer failed: grabFrame() returned no frame (sensor/driver issue)");
    return false;
  }

  uint8_t *copy = (uint8_t *)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM);
  bool ok = false;
  if (copy) {
    memcpy(copy, fb->buf, fb->len);
    *outBuf = copy;
    *outLen = fb->len;
    ok = true;
  } else {
    deviceLog("[capture] snapToBuffer failed: out of PSRAM allocating %u bytes", (unsigned)fb->len);
  }

  esp_camera_fb_return(fb);

  if (size != streamFramesize) {
    restoreStreamMode(s);
  }

  xSemaphoreGive(cameraBusy);
  return ok;
}
