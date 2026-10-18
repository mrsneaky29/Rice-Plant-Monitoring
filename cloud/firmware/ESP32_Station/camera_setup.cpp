#include "camera_setup.h"
#include <Arduino.h>
#include <string.h>
#include <driver/gpio.h>
#include "board_config.h"
#include "device_log.h"

static const char *sensorName = "unknown";
static uint16_t sensorPid = 0;
static framesize_t maxFramesize = FRAMESIZE_UXGA;
static framesize_t streamFramesize = FRAMESIZE_SVGA;
static int streamQuality = 12;
static int captureQuality = 10;
static uint32_t streamMinIntervalMs = 0;

static bool targetIsOv5640() {
#if defined(CAMERA_SENSOR_OV5640)
  return true;
#else
  return false;
#endif
}

static void applySensorTuning(sensor_t *s) {
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }

  if (s->id.PID == OV5640_PID) {
    s->set_vflip(s, 0);
    s->set_hmirror(s, 0);
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_sharpness(s, 1);
    s->set_special_effect(s, 0);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_gain_ctrl(s, 1);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif
}

static void configureForSensor(sensor_t *s) {
  sensorPid = s->id.PID;

  maxFramesize = FRAMESIZE_UXGA;
  streamFramesize = FRAMESIZE_SVGA;
  streamQuality = 12;
  captureQuality = 10;
  streamMinIntervalMs = 0;
  sensorName = "OV2640 (or unrecognized)";

  if (s->id.PID == OV5640_PID) {
    maxFramesize = FRAMESIZE_QXGA;
    streamFramesize = FRAMESIZE_VGA;
    streamQuality = 12;
    captureQuality = 10;
    streamMinIntervalMs = 66;
    sensorName = "OV5640";
  } else if (s->id.PID == OV3660_PID) {
    maxFramesize = FRAMESIZE_QXGA;
    sensorName = "OV3660";
  }

  applySensorTuning(s);

  deviceLog("Camera sensor detected: %s (PID 0x%04x) -- full-res captures at %dx%d, streaming at %dx%d",
            sensorName, s->id.PID, resolution[maxFramesize].width, resolution[maxFramesize].height,
            resolution[streamFramesize].width, resolution[streamFramesize].height);

  // Idle default is low-res stream mode; full-res is switched in only for captures.
  s->set_framesize(s, streamFramesize);
  s->set_quality(s, streamQuality);
}

static camera_config_t buildBaseConfig() {
  camera_config_t config;
  memset(&config, 0, sizeof(config));
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;


  bool ov5640Target = targetIsOv5640();
  config.xclk_freq_hz = ov5640Target ? 10000000 : 20000000;
  config.frame_size = ov5640Target ? FRAMESIZE_QXGA : FRAMESIZE_UXGA;

  if (psramFound()) {
    config.jpeg_quality = ov5640Target ? 12 : 10;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
    deviceLog("PSRAM not found -- capturing at SVGA in DRAM");
  }

  return config;
}

bool cameraSetupBegin() {
  gpio_deep_sleep_hold_dis();
#if defined(LED_GPIO_NUM) && LED_GPIO_NUM >= 0
  gpio_hold_dis((gpio_num_t)LED_GPIO_NUM);
#endif
#if defined(PWDN_GPIO_NUM) && PWDN_GPIO_NUM >= 0
  gpio_hold_dis((gpio_num_t)PWDN_GPIO_NUM);
#endif
#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  camera_config_t config = buildBaseConfig();
  deviceLog("Camera init: XCLK=%d Hz, fb_count=%d, PSRAM=%s",
            (int)config.xclk_freq_hz, (int)config.fb_count, psramFound() ? "yes" : "no");
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    deviceLog("Camera init failed with error 0x%x", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    deviceLog("Camera init succeeded but sensor handle is NULL");
    esp_camera_deinit();
    return false;
  }

  // Runtime OV5640 detection: if we booted at 20 MHz and see OV5640, drop
  // XCLK once for DVP timing margin (FB-OVF on marginal ribbon cables).
  if (s->id.PID == OV5640_PID && config.xclk_freq_hz > 10000000) {
    deviceLog("OV5640 detected -- reinitializing at 10MHz XCLK for better DVP bus signal margin (was 20MHz)");
    esp_camera_deinit();
    camera_config_t retryConfig = config;
    retryConfig.xclk_freq_hz = 10000000;
    retryConfig.frame_size = FRAMESIZE_QXGA;
    esp_err_t err2 = esp_camera_init(&retryConfig);
    if (err2 == ESP_OK) {
      s = esp_camera_sensor_get();
    } else {
      deviceLog("10MHz reinit failed (0x%x) -- reverting to 20MHz", err2);
      if (esp_camera_init(&config) != ESP_OK) {
        return false;
      }
      s = esp_camera_sensor_get();
    }
  }

  if (!s) {
    deviceLog("Camera sensor handle is NULL after init");
    return false;
  }

  configureForSensor(s);
  return true;
}

const char *cameraSetupSensorName() {
  return sensorName;
}

uint16_t cameraSetupSensorPid() {
  return sensorPid;
}

framesize_t cameraSetupMaxFramesize() {
  return maxFramesize;
}

framesize_t cameraSetupStreamFramesize() {
  return streamFramesize;
}

int cameraSetupStreamQuality() {
  return streamQuality;
}

int cameraSetupCaptureQuality() {
  return captureQuality;
}

uint32_t cameraSetupStreamMinIntervalMs() {
  return streamMinIntervalMs;
}
