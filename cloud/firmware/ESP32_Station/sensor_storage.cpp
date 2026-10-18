#include "sensor_storage.h"
#include <Arduino.h>
#include "FS.h"
#include "SD_MMC.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sd_storage.h"
#include "time_keeper.h"

#define DCIM_DIR "/DCIM"
#define SENSOR_FILE_NAME "sensors.csv"

static bool nameLooksSafe(const char *s, size_t maxLen) {
  if (!s || !s[0] || strlen(s) > maxLen) {
    return false;
  }
  if (strstr(s, "..") || strchr(s, '/') || strchr(s, '\\')) {
    return false;
  }
  return true;
}

static bool appendReading(const char *kind, float a, float b) {
  if (!sdStorageIsMounted() || sdStorageIsFull()) {
    return false;
  }

  char date[16], hhmmss[8];
  if (!timeKeeperLocalDate(date, sizeof(date)) || !timeKeeperLocalTimeCompact(hhmmss, sizeof(hhmmss))) {
    return false;  // clock not set; skip rather than file it under a bogus date
  }

  // Shared lock across every module that touches the SD card -- see the
  // comment on sdStorageLock() in sd_storage.h for why this can't be its
  // own independent mutex.
  if (!sdStorageLock(2000)) {
    return false;
  }

  char dirPath[32];
  snprintf(dirPath, sizeof(dirPath), DCIM_DIR "/%s", date);
  if (!SD_MMC.exists(dirPath)) {
    SD_MMC.mkdir(dirPath);
  }
  char path[64];
  snprintf(path, sizeof(path), "%s/%s", dirPath, SENSOR_FILE_NAME);

  bool ok = false;
  File f = SD_MMC.open(path, FILE_APPEND);
  if (f) {
    char line[72];
    int n = snprintf(line, sizeof(line), "%s,%s,%.4f,%.4f\n", hhmmss, kind, a, b);
    ok = n > 0 && f.write((const uint8_t *)line, n) == (size_t)n;
    f.close();
  }

  sdStorageUnlock();
  return ok;
}

bool sensorStorageAppend(float tempC, float humidity) { return sensorStorageAppendDht(tempC, humidity); }
bool sensorStorageAppendDht(float tempC, float humidity) { return appendReading("dht22", tempC, humidity); }
bool sensorStorageAppendSoil(float volts, float percent) { return appendReading("soil", volts, percent); }

// Row format on disk is "HHMMSS,tempC,humidity" -- compact and directly
// comparable against fromTime/toTime (same "HHMMSS" scheme the photo
// filters use), reformatted with colons only for the JSON response.
static bool passesTimeFilter(const char *hhmmss, const char *fromTime, const char *toTime) {
  bool hasFrom = fromTime && fromTime[0];
  bool hasTo = toTime && toTime[0];
  if (!hasFrom && !hasTo) {
    return true;
  }
  if (hasFrom && strcmp(hhmmss, fromTime) < 0) {
    return false;
  }
  if (hasTo && strcmp(hhmmss, toTime) > 0) {
    return false;
  }
  return true;
}

bool sensorStorageReadingsJson(const char *date, int page, int per, const char *fromTime, const char *toTime, char *out, size_t outSize) {
  if (!out || outSize < 64 || !nameLooksSafe(date, 15)) {
    return false;
  }
  if (per < 1) per = 20;
  if (per > 2000) per = 2000;
  if (page < 0) page = 0;

  char path[64];
  snprintf(path, sizeof(path), DCIM_DIR "/%s/%s", date, SENSOR_FILE_NAME);

  if (!sdStorageIsMounted()) {
    snprintf(out, outSize, "{\"date\":\"%s\",\"total\":0,\"page\":0,\"per\":%d,\"rows\":[]}", date, per);
    return true;
  }

  // Pass 1: count matching rows.
  int total = 0;
  {
    File f = SD_MMC.open(path, FILE_READ);
    if (f) {
      while (f.available()) {
        String line = f.readStringUntil('\n');
        if (line.length() < 6) continue;
        char hhmmss[8];
        snprintf(hhmmss, sizeof(hhmmss), "%.6s", line.c_str());
        if (passesTimeFilter(hhmmss, fromTime, toTime)) {
          total++;
        }
      }
      f.close();
    }
  }

  int end = total - page * per;    // exclusive, counted from oldest
  int start = end - per;
  if (start < 0) start = 0;
  if (end < 0) end = 0;

  size_t pos = 0;
  pos += snprintf(out + pos, outSize - pos, "{\"date\":\"%s\",\"total\":%d,\"page\":%d,\"per\":%d,\"rows\":[", date, total, page, per);

  // Pass 2: emit the requested window.
  File f = SD_MMC.open(path, FILE_READ);
  if (f) {
    int idx = 0;
    bool first = true;
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() < 6) continue;
      char hhmmss[8];
      snprintf(hhmmss, sizeof(hhmmss), "%.6s", line.c_str());
      if (!passesTimeFilter(hhmmss, fromTime, toTime)) continue;
      if (idx >= end) break;
      if (idx >= start) {
        int c1 = line.indexOf(',');
        int c2 = c1 >= 0 ? line.indexOf(',', c1 + 1) : -1;
        int c3 = c2 >= 0 ? line.indexOf(',', c2 + 1) : -1;
        if (c1 > 0 && c2 > c1) {
          String kind, a, b;
          if (c3 > c2) {
            kind = line.substring(c1 + 1, c2);
            a = line.substring(c2 + 1, c3);
            b = line.substring(c3 + 1);
          } else {
            kind = "dht22"; // legacy HHMMSS,temp,humidity row
            a = line.substring(c1 + 1, c2);
            b = line.substring(c2 + 1);
          }
          char timeDisp[10];
          snprintf(timeDisp, sizeof(timeDisp), "%c%c:%c%c:%c%c", hhmmss[0], hhmmss[1], hhmmss[2], hhmmss[3], hhmmss[4], hhmmss[5]);
          int n;
          if (kind == "soil") {
            n = snprintf(out + pos, outSize - pos, "%s{\"t\":\"%s\",\"role\":\"soil\",\"volts\":%s,\"soil_percent\":%s}", first ? "" : ",", timeDisp, a.c_str(), b.c_str());
          } else {
            n = snprintf(out + pos, outSize - pos, "%s{\"t\":\"%s\",\"role\":\"dht22\",\"temperature_c\":%s,\"humidity_percent\":%s}", first ? "" : ",", timeDisp, a.c_str(), b.c_str());
          }
          if (n < 0 || (size_t)n >= outSize - pos - 4) break;
          pos += n;
          first = false;
        }
      }
      idx++;
    }
    f.close();
  }

  pos += snprintf(out + pos, outSize - pos, "]}");
  return true;
}

int sensorStorageEraseAll() {
  if (!sdStorageIsMounted() || !sdStorageLock(5000)) {
    return 0;
  }

  int removed = 0;
  File root = SD_MMC.open(DCIM_DIR);
  if (root && root.isDirectory()) {
    File entry = root.openNextFile();
    while (entry) {
      if (entry.isDirectory()) {
        char path[64];
        snprintf(path, sizeof(path), "%s/%s/%s", DCIM_DIR, entry.name(), SENSOR_FILE_NAME);
        if (SD_MMC.exists(path) && SD_MMC.remove(path)) {
          removed++;
        }
      }
      entry = root.openNextFile();
    }
    root.close();
  }

  sdStorageUnlock();
  return removed;
}
