#include "device_log.h"
#include <Arduino.h>
#include <stdarg.h>
#include <string.h>
#include "FS.h"
#include "SD_MMC.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sd_storage.h"
#include "time_keeper.h"

#define LOG_PATH "/LOG.txt"
#define LOG_MAX_BYTES (256UL * 1024)
#define RAM_LOG_LINES 120
#define RAM_LOG_COLS 192

static char ramLog[RAM_LOG_LINES][RAM_LOG_COLS];
static uint16_t ramHead = 0;
static uint16_t ramCount = 0;
static SemaphoreHandle_t ramMutex = NULL;

void deviceLogBegin() {
  if (!ramMutex) {
    ramMutex = xSemaphoreCreateMutex();
  }
}

static void formatStamp(char *stamp, size_t stampSize) {
  stamp[0] = '\0';
  timeKeeperLocalDateTime(stamp, stampSize);
  if (stamp[0]) {
    return;
  }
  snprintf(stamp, stampSize, "boot+%lus", (unsigned long)(millis() / 1000));
}

static void appendRamLine(const char *stamped) {
  if (!ramMutex || xSemaphoreTake(ramMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return;
  }
  snprintf(ramLog[ramHead], RAM_LOG_COLS, "%s", stamped);
  ramHead = (ramHead + 1) % RAM_LOG_LINES;
  if (ramCount < RAM_LOG_LINES) {
    ramCount++;
  }
  xSemaphoreGive(ramMutex);
}

static void writeSdLine(const char *stamped) {
  if (!sdStorageIsMounted() || !sdStorageLock(500)) {
    return;
  }
  if (SD_MMC.exists(LOG_PATH)) {
    File check = SD_MMC.open(LOG_PATH, FILE_READ);
    if (check) {
      bool tooBig = check.size() > LOG_MAX_BYTES;
      check.close();
      if (tooBig) {
        SD_MMC.remove(LOG_PATH);
      }
    }
  }
  File f = SD_MMC.open(LOG_PATH, FILE_APPEND);
  if (f) {
    f.printf("%s\n", stamped);
    f.close();
  }
  sdStorageUnlock();
}

static void emitLine(const char *text) {
  char stamp[24];
  formatStamp(stamp, sizeof(stamp));
  char stamped[RAM_LOG_COLS];
  snprintf(stamped, sizeof(stamped), "[%s] %s", stamp, text);
  appendRamLine(stamped);
  writeSdLine(stamped);
}

void deviceLog(const char *fmt, ...) {
  char msg[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  emitLine(msg);
}

void deviceLogRaw(const char *tag, const char *text) {
  if (!text) {
    text = "";
  }
  size_t tagLen = tag ? strlen(tag) : 0;
  size_t textLen = strlen(text);
  size_t bufLen = tagLen + textLen + 4;
  char *line = (char *)heap_caps_malloc(bufLen, MALLOC_CAP_SPIRAM);
  if (!line) {
    char shortMsg[192];
    snprintf(shortMsg, sizeof(shortMsg), "[%s] %s", tag ? tag : "", text);
    emitLine(shortMsg);
    return;
  }
  snprintf(line, bufLen, "[%s] %s", tag ? tag : "", text);
  emitLine(line);
  free(line);
}

bool deviceLogTail(int maxLines, char *out, size_t outSize) {
  if (!out || outSize < 8) {
    return false;
  }
  out[0] = '\0';
  if (maxLines < 1) {
    maxLines = 1;
  }
  if (maxLines > RAM_LOG_LINES) {
    maxLines = RAM_LOG_LINES;
  }
  if (!ramMutex || xSemaphoreTake(ramMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    return false;
  }
  if (ramCount == 0) {
    xSemaphoreGive(ramMutex);
    return false;
  }

  int start = (int)ramCount > maxLines ? (int)ramCount - maxLines : 0;
  size_t used = 0;
  for (int i = start; i < (int)ramCount && used + 2 < outSize; i++) {
    uint16_t idx = (uint16_t)((ramHead + RAM_LOG_LINES - ramCount + i) % RAM_LOG_LINES);
    size_t n = strlen(ramLog[idx]);
    if (used + n + 2 >= outSize) {
      n = outSize - used - 2;
    }
    memcpy(out + used, ramLog[idx], n);
    used += n;
    out[used++] = '\n';
  }
  out[used] = '\0';
  xSemaphoreGive(ramMutex);
  return used > 0;
}
