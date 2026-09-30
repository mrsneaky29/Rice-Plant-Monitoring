#include "capture_timer.h"
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "photo_capture.h"
#include "sd_storage.h"
#include "time_keeper.h"
#include "device_log.h"

static volatile bool timerEnabled = false;
static volatile uint32_t timerIntervalSeconds = 10;
// Shared with the task so enabling / changing the interval restarts the
// countdown from that moment.
static volatile uint32_t elapsedSeconds = 0;

static volatile bool windowEnabled = false;
static volatile uint16_t windowStartMin = 0;
static volatile uint16_t windowEndMin = 1439;

bool captureTimerInWindow() {
  if (!windowEnabled) {
    return true;
  }
  int now = timeKeeperLocalMinuteOfDay();
  if (now < 0) {
    return false;  // clock not set yet; hold back rather than guess
  }
  uint16_t start = windowStartMin, end = windowEndMin;
  if (start == end) {
    return true;  // zero-length window == no restriction
  }
  if (start < end) {
    return now >= start && now < end;
  }
  return now >= start || now < end;  // wraps past midnight
}

static void captureTimerTask(void *pvParameters) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (!timerEnabled || !captureTimerInWindow()) {
      elapsedSeconds = 0;
      continue;
    }

    elapsedSeconds++;
    if (elapsedSeconds < timerIntervalSeconds) {
      continue;
    }
    elapsedSeconds = 0;

    if (sdStorageIsFull()) {
      timerEnabled = false;
      continue;
    }

    char filename[48];
    bool captured = photoCaptureSnap(filename, sizeof(filename));
    if (captured) {
      deviceLog("[timer] scheduled capture saved %s", filename);
    } else {
      deviceLog("[timer] scheduled capture skipped (camera busy or SD full)");
    }
  }
}

void captureTimerBegin() {
  xTaskCreatePinnedToCore(captureTimerTask, "captureTimer", 4096, NULL, 1, NULL, 1);
}

void captureTimerSetEnabled(bool enabled) {
  elapsedSeconds = 0;
  timerEnabled = enabled;
}

void captureTimerSetIntervalSeconds(uint32_t seconds) {
  if (seconds < 1) {
    seconds = 1;
  }
  timerIntervalSeconds = seconds;
  elapsedSeconds = 0;
}

bool captureTimerIsEnabled() {
  return timerEnabled;
}

uint32_t captureTimerIntervalSeconds() {
  return timerIntervalSeconds;
}

void captureTimerSetWindow(bool enabled, uint16_t startMin, uint16_t endMin) {
  if (startMin > 1439) startMin = 1439;
  if (endMin > 1439) endMin = 1439;
  windowStartMin = startMin;
  windowEndMin = endMin;
  windowEnabled = enabled;
  elapsedSeconds = 0;
}

bool captureTimerWindowEnabled() {
  return windowEnabled;
}

uint16_t captureTimerWindowStart() {
  return windowStartMin;
}

uint16_t captureTimerWindowEnd() {
  return windowEndMin;
}
