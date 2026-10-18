#include "capture_timer.h"
#include <Arduino.h>
#include <time.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "photo_capture.h"
#include "sd_storage.h"
#include "time_keeper.h"
#include "device_log.h"
#include "cloud_device.h"
#include "cloud_run_mode.h"
#include "capture_schedule_logic.h"

static volatile bool timerEnabled = false;
static volatile uint32_t timerIntervalSeconds = 10;
static volatile bool windowEnabled = false;
static volatile uint16_t windowStartMin = 0;
static volatile uint16_t windowEndMin = 1439;
static portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
// ESP32 RTC slow memory survives deep sleep. Keeping an absolute epoch deadline
// prevents capture intervals from drifting by the time spent syncing/uploading.
RTC_DATA_ATTR static uint32_t nextCaptureEpoch = 0;
// Fallback for periods before NTP establishes wall-clock time.
static volatile uint32_t unsyncedElapsedSeconds = 0;

static uint32_t nowEpoch() {
  time_t now = time(NULL);
  return now >= 1600000000 ? (uint32_t)now : 0;
}

static bool inWindowAt(uint32_t epoch) {
  if (!windowEnabled) return true;
  if (epoch < 1600000000) return false;
  int now = timeKeeperLocalMinuteOfDay();
  if (now < 0) return false;
  uint16_t start = windowStartMin, end = windowEndMin;
  if (start == end) return true;
  if (start < end) return now >= start && now < end;
  return now >= start || now < end;
}

bool captureTimerInWindow() {
  return !windowEnabled || inWindowAt(nowEpoch());
}

static uint32_t secondsUntilWindowStart() {
  int minute = timeKeeperLocalMinuteOfDay();
  if (minute < 0) return 0;
  time_t now = time(NULL);
  struct tm utc;
  gmtime_r(&now, &utc);
  int secondOfMinute = utc.tm_sec;
  int deltaMinutes = (int)windowStartMin - minute;
  if (deltaMinutes <= 0) deltaMinutes += 1440;
  uint32_t result = (uint32_t)deltaMinutes * 60;
  return result > (uint32_t)secondOfMinute ? result - (uint32_t)secondOfMinute : 1;
}

static void restartDeadline() {
  uint32_t now = nowEpoch();
  portENTER_CRITICAL(&timerMux);
  nextCaptureEpoch = now ? now + timerIntervalSeconds : 0;
  unsyncedElapsedSeconds = 0;
  portEXIT_CRITICAL(&timerMux);
}

// Called once on normal startup after loading persisted settings. It preserves
// the RTC deadline on timer deep-sleep wakes, but initializes it on power-on.
void captureTimerRestoreSchedule(bool enabled, uint32_t intervalSeconds,
                                 bool winEnabled, uint16_t startMin,
                                 uint16_t endMin) {
  if (intervalSeconds < 1) intervalSeconds = 1;
  if (startMin > 1439) startMin = 1439;
  if (endMin > 1439) endMin = 1439;
  timerIntervalSeconds = intervalSeconds;
  windowEnabled = winEnabled;
  windowStartMin = startMin;
  windowEndMin = endMin;
  timerEnabled = enabled;
  // A retained overdue deadline is meaningful: on a timer wake it represents
  // the scheduled event that caused deep sleep to end. Consume it once below.
  if (enabled && !nextCaptureEpoch) {
    restartDeadline();
  }
}

bool captureTimerConsumeDue() {
  if (!timerEnabled) return false;
  uint32_t now = nowEpoch();
  bool allowed = captureTimerInWindow();
  uint32_t waitForWindow = windowEnabled && !allowed && now ? secondsUntilWindowStart() : 0;
  portENTER_CRITICAL(&timerMux);
  if (!now) {
    if (++unsyncedElapsedSeconds < timerIntervalSeconds) {
      portEXIT_CRITICAL(&timerMux);
      return false;
    }
    unsyncedElapsedSeconds = 0;
    portEXIT_CRITICAL(&timerMux);
    return allowed;
  }
  CaptureScheduleTransition transition = captureScheduleTransition(
      now, nextCaptureEpoch, timerIntervalSeconds, allowed, waitForWindow);
  nextCaptureEpoch = transition.deadline;
  unsyncedElapsedSeconds = 0;
  portEXIT_CRITICAL(&timerMux);
  return transition.due;
}

static void captureTimerTask(void *pvParameters) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    // The main cloud loop owns the timer-wake and active cloud upload cycles.
    // This prevents the scheduler task from racing or consuming deadlines concurrently.
    if (cloudDeviceTimerWakePending() || cloudDeviceCycleActive()) continue;
    if (!captureTimerConsumeDue()) continue;
#if RICE_CLOUD_STATUS_ONLY
    // Keep scheduled wake/status cycles without mounting or writing the card.
    cloudDeviceScheduledWake();
    continue;
#else
    if (sdStorageIsFull()) {
      captureTimerSetEnabled(false);
      continue;
    }
    if (cloudDeviceSleepEnabled() && cloudDeviceMaintenanceRemainingSeconds() == 0) {
      cloudDeviceScheduledWake();
    } else {
      char filename[48];
      bool captured = photoCaptureSnap(filename, sizeof(filename));
      if (captured) deviceLog("[timer] scheduled capture saved %s", filename);
      else deviceLog("[timer] scheduled capture skipped (camera busy or SD full)");
    }
#endif
  }
}

void captureTimerBegin() {
  xTaskCreatePinnedToCore(captureTimerTask, "captureTimer", 4096, NULL, 1, NULL, 1);
}

void captureTimerSetEnabled(bool enabled) {
  bool changed = timerEnabled != enabled;
  timerEnabled = enabled;
  if (changed) restartDeadline();
}

void captureTimerSetIntervalSeconds(uint32_t seconds) {
  if (seconds < 1) seconds = 1;
  bool changed = timerIntervalSeconds != seconds;
  timerIntervalSeconds = seconds;
  if (changed) restartDeadline();
}

bool captureTimerIsEnabled() { return timerEnabled; }
uint32_t captureTimerIntervalSeconds() { return timerIntervalSeconds; }

uint32_t captureTimerSecondsUntilNext() {
  if (!timerEnabled) return 0;
  uint32_t now = nowEpoch();
  if (!now) {
    uint32_t elapsed = unsyncedElapsedSeconds;
    return elapsed >= timerIntervalSeconds ? 1 : timerIntervalSeconds - elapsed;
  }
  if (!inWindowAt(now)) {
    uint32_t wait = secondsUntilWindowStart();
    return wait > 0 ? wait : 1;
  }
  int32_t remaining = (int32_t)(nextCaptureEpoch - now);
  return remaining > 0 ? (uint32_t)remaining : 1;
}

void captureTimerSetWindow(bool enabled, uint16_t startMin, uint16_t endMin) {
  if (startMin > 1439) startMin = 1439;
  if (endMin > 1439) endMin = 1439;
  bool changed = windowEnabled != enabled || windowStartMin != startMin || windowEndMin != endMin;
  windowStartMin = startMin;
  windowEndMin = endMin;
  windowEnabled = enabled;
  if (changed) restartDeadline();
}

bool captureTimerWindowEnabled() { return windowEnabled; }
uint16_t captureTimerWindowStart() { return windowStartMin; }
uint16_t captureTimerWindowEnd() { return windowEndMin; }
