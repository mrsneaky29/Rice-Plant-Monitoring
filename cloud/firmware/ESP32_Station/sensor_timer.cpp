#include "sensor_timer.h"
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dht_sensor.h"
#include "sensor_storage.h"
#include "time_keeper.h"
#include "device_log.h"
#include "settings_store.h"
#include "ads1115.h"

static volatile bool timerEnabled = false;
static volatile uint32_t timerIntervalSeconds = 60;
static volatile uint32_t elapsedSeconds = 0;

static volatile bool windowEnabled = false;
static volatile uint16_t windowStartMin = 0;
static volatile uint16_t windowEndMin = 1439;

bool sensorTimerInWindow() {
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

static void sensorTimerTask(void *pvParameters) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (!timerEnabled || !sensorTimerInWindow()) {
      elapsedSeconds = 0;
      continue;
    }

    elapsedSeconds++;
    if (elapsedSeconds < timerIntervalSeconds) {
      continue;
    }
    elapsedSeconds = 0;

    StationRole role = settingsStationRole();
    if (role == STATION_ROLE_SOIL) {
      float v;
      if (ads1115Present() && ads1115ReadA0(&v)) {
        float pct = ads1115MoisturePercent(v);
        if (sensorStorageAppendSoil(v, pct)) deviceLog("[soil] scheduled reading: %.3fV %.1f%%", v, pct);
      }
    } else if (role == STATION_ROLE_DHT22) {
      float t, h;
      if (dhtSensorRead(&t, &h) && sensorStorageAppendDht(t, h)) {
        deviceLog("[dht] scheduled reading: %.1fC %.1f%% RH", t, h);
      }
    }
  }
}

void sensorTimerBegin() {
  xTaskCreatePinnedToCore(sensorTimerTask, "sensorTimer", 4096, NULL, 1, NULL, 1);
}

void sensorTimerSetEnabled(bool enabled) {
  elapsedSeconds = 0;
  timerEnabled = enabled;
}

void sensorTimerSetIntervalSeconds(uint32_t seconds) {
  if (seconds < 1) {
    seconds = 1;
  }
  timerIntervalSeconds = seconds;
  elapsedSeconds = 0;
}

bool sensorTimerIsEnabled() {
  return timerEnabled;
}

uint32_t sensorTimerIntervalSeconds() {
  return timerIntervalSeconds;
}

void sensorTimerSetWindow(bool enabled, uint16_t startMin, uint16_t endMin) {
  if (startMin > 1439) startMin = 1439;
  if (endMin > 1439) endMin = 1439;
  windowStartMin = startMin;
  windowEndMin = endMin;
  windowEnabled = enabled;
  elapsedSeconds = 0;
}

bool sensorTimerWindowEnabled() {
  return windowEnabled;
}

uint16_t sensorTimerWindowStart() {
  return windowStartMin;
}

uint16_t sensorTimerWindowEnd() {
  return windowEndMin;
}
