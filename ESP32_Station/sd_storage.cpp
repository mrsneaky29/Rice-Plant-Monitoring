#include "sd_storage.h"
#include <Arduino.h>
#include "FS.h"
#include "SD_MMC.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "time_keeper.h"
#include "device_log.h"

#define DCIM_DIR "/DCIM"
#define UNSORTED_NAME "UNSORTED"
#define MIN_FREE_BYTES (1ULL * 1024 * 1024)  // 1MB safety margin before refusing writes

static bool mounted = false;
static uint32_t photoCount = 0;
static uint32_t unsortedIndex = 1;
static SemaphoreHandle_t sdMutex = NULL;
static volatile bool erasing = false;

static bool nameLooksSafe(const char *s, size_t maxLen) {
  if (!s || !s[0] || strlen(s) > maxLen) {
    return false;
  }
  if (strstr(s, "..") || strchr(s, '/') || strchr(s, '\\')) {
    return false;
  }
  return true;
}

static bool nameIsJpeg(const char *name) {
  if (!name) return false;
  size_t len = strlen(name);
  return len > 4 && name[len - 4] == '.' &&
         (name[len - 3] == 'j' || name[len - 3] == 'J') &&
         (name[len - 2] == 'p' || name[len - 2] == 'P') &&
         (name[len - 1] == 'g' || name[len - 1] == 'G');
}

// Old firmware saved flat IMG_xxxxx.jpg files in /DCIM. Move them into
// UNSORTED so they show up in the gallery. Re-opens the directory after
// each rename because renaming invalidates the iterator.
static void migrateLegacyPhotos() {
  for (int guard = 0; guard < 2000; guard++) {
    File dir = SD_MMC.open(DCIM_DIR);
    if (!dir) {
      return;
    }
    char found[40] = { 0 };
    File entry = dir.openNextFile();
    while (entry) {
      if (!entry.isDirectory() && nameIsJpeg(entry.name()) && strstr(entry.name(), "IMG_")) {
        snprintf(found, sizeof(found), "%s", entry.name());
        entry.close();
        break;
      }
      entry = dir.openNextFile();
    }
    dir.close();
    if (!found[0]) {
      return;
    }
    char from[64], to[64];
    snprintf(from, sizeof(from), DCIM_DIR "/%s", found);
    snprintf(to, sizeof(to), DCIM_DIR "/" UNSORTED_NAME "/%s", found);
    if (!SD_MMC.rename(from, to)) {
      deviceLog("[sd] legacy migrate failed for %s", from);
      return;
    }
  }
}

static void scanExistingPhotos() {
  photoCount = 0;
  unsortedIndex = 1;

  File root = SD_MMC.open(DCIM_DIR);
  if (!root || !root.isDirectory()) {
    return;
  }

  File entry = root.openNextFile();
  while (entry) {
    if (entry.isDirectory()) {
      const char *dname = entry.name();
      bool isUnsorted = strcmp(dname, UNSORTED_NAME) == 0;
      char sub[64];
      snprintf(sub, sizeof(sub), DCIM_DIR "/%s", dname);
      File subdir = SD_MMC.open(sub);
      if (subdir) {
        File f = subdir.openNextFile();
        while (f) {
          if (!f.isDirectory() && nameIsJpeg(f.name())) {
            photoCount++;
            if (isUnsorted) {
              const char *img = strstr(f.name(), "IMG_");
              if (img) {
                uint32_t idx = (uint32_t)atoi(img + 4);
                if (idx >= unsortedIndex) {
                  unsortedIndex = idx + 1;
                }
              }
            }
          }
          f = subdir.openNextFile();
        }
        subdir.close();
      }
    }
    entry = root.openNextFile();
  }
  root.close();
}

bool sdStorageBegin() {
  if (!sdMutex) {
    sdMutex = xSemaphoreCreateMutex();
  }

  mounted = SD_MMC.begin("/sdcard", true, true);
  if (!mounted) {
    deviceLog("SD Card Mount Failed");
    return false;
  }

  sdcard_type_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    deviceLog("No SD Card attached");
    mounted = false;
    return false;
  }

  if (!SD_MMC.exists(DCIM_DIR)) {
    SD_MMC.mkdir(DCIM_DIR);
  }
  if (!SD_MMC.exists(DCIM_DIR "/" UNSORTED_NAME)) {
    SD_MMC.mkdir(DCIM_DIR "/" UNSORTED_NAME);
  }

  migrateLegacyPhotos();
  scanExistingPhotos();
  deviceLog("SD Card mounted. %u existing photos", (unsigned)photoCount);
  return true;
}

bool sdStorageLock(uint32_t timeoutMs) {
  if (!sdMutex) {
    return false;
  }
  return xSemaphoreTake(sdMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void sdStorageUnlock() {
  if (sdMutex) {
    xSemaphoreGive(sdMutex);
  }
}

bool sdStorageIsMounted() {
  return mounted;
}

uint64_t sdStorageFreeBytes() {
  if (!mounted) {
    return 0;
  }
  return SD_MMC.totalBytes() - SD_MMC.usedBytes();
}

uint64_t sdStorageTotalBytes() {
  if (!mounted) {
    return 0;
  }
  return SD_MMC.totalBytes();
}

bool sdStorageIsFull() {
  if (!mounted) {
    return true;
  }
  return sdStorageFreeBytes() < MIN_FREE_BYTES;
}

uint32_t sdStoragePhotoCount() {
  return photoCount;
}

bool sdStorageSavePhoto(const uint8_t *data, size_t len, char *outFilename, size_t outFilenameSize) {
  if (!mounted || erasing || sdStorageIsFull()) {
    return false;
  }
  if (!sdStorageLock(3000)) {
    return false;
  }

  char date[16];
  char name[36];
  bool haveTime = timeKeeperLocalDate(date, sizeof(date));
  if (!haveTime) {
    snprintf(date, sizeof(date), UNSORTED_NAME);
  }

  char dirPath[32];
  snprintf(dirPath, sizeof(dirPath), DCIM_DIR "/%s", date);
  if (!SD_MMC.exists(dirPath)) {
    SD_MMC.mkdir(dirPath);
  }

  char path[96];
  bool haveName = false;
  if (haveTime) {
    char stamp[32];
    timeKeeperLocalStampMs(stamp, sizeof(stamp));
    snprintf(name, sizeof(name), "%s.jpg", stamp);
    snprintf(path, sizeof(path), "%s/%s", dirPath, name);
    // Millisecond stamps can still collide back-to-back; fall back to a suffix.
    for (int i = 1; i <= 9 && SD_MMC.exists(path); i++) {
      snprintf(name, sizeof(name), "%s_%d.jpg", stamp, i);
      snprintf(path, sizeof(path), "%s/%s", dirPath, name);
    }
    haveName = !SD_MMC.exists(path);
  }
  if (!haveName) {
    snprintf(name, sizeof(name), "IMG_%05u.jpg", (unsigned)unsortedIndex++);
    snprintf(path, sizeof(path), "%s/%s", dirPath, name);
  }

  File file = SD_MMC.open(path, FILE_WRITE);
  if (!file) {
    deviceLog("Failed to open %s for writing", path);
    sdStorageUnlock();
    return false;
  }

  size_t written = file.write(data, len);
  file.close();

  if (written != len) {
    deviceLog("Short write on %s (%u of %u bytes)", path, (unsigned)written, (unsigned)len);
    SD_MMC.remove(path);
    sdStorageUnlock();
    return false;
  }

  if (outFilename && outFilenameSize > 0) {
    snprintf(outFilename, outFilenameSize, "%s/%s", date, name);
  }

  photoCount++;
  sdStorageUnlock();
  return true;
}

bool sdStorageDatesJson(char *out, size_t outSize) {
  if (!mounted || !out || outSize < 32) {
    return false;
  }
  size_t pos = 0;
  pos += snprintf(out + pos, outSize - pos, "{\"dates\":[");

  File root = SD_MMC.open(DCIM_DIR);
  if (root && root.isDirectory()) {
    bool first = true;
    File entry = root.openNextFile();
    while (entry) {
      if (entry.isDirectory()) {
        int n = snprintf(out + pos, outSize - pos, "%s\"%s\"", first ? "" : ",", entry.name());
        if (n < 0 || (size_t)n >= outSize - pos - 4) {
          break;  // out of buffer; return what fits
        }
        pos += n;
        first = false;
      }
      entry = root.openNextFile();
    }
    root.close();
  }

  pos += snprintf(out + pos, outSize - pos, "]}");
  return true;
}

// Extracts "HHMMSS" from a name in the "YYYY-MM-DD__HH-MM-SS-mmm.jpg" scheme.
// Returns false (name unchanged/untouched) for legacy "IMG_xxxxx.jpg" names,
// which are treated as always-matching by the time filter.
static bool extractHHMMSS(const char *name, char *out6) {
  // Fixed layout: 0123456789__HH-MM-SS-mmm.jpg
  if (strlen(name) < 23 || name[4] != '-' || name[7] != '-' || name[10] != '_' || name[11] != '_' || name[14] != '-' || name[17] != '-') {
    return false;
  }
  out6[0] = name[12]; out6[1] = name[13];
  out6[2] = name[15]; out6[3] = name[16];
  out6[4] = name[18]; out6[5] = name[19];
  out6[6] = '\0';
  return true;
}

static bool passesTimeFilter(const char *name, const char *fromTime, const char *toTime) {
  bool hasFrom = fromTime && fromTime[0];
  bool hasTo = toTime && toTime[0];
  if (!hasFrom && !hasTo) {
    return true;
  }
  char hhmmss[7];
  if (!extractHHMMSS(name, hhmmss)) {
    return true;  // no embedded time; don't exclude it
  }
  if (hasFrom && strcmp(hhmmss, fromTime) < 0) {
    return false;
  }
  if (hasTo && strcmp(hhmmss, toTime) > 0) {
    return false;
  }
  return true;
}

int sdStoragePhotoCountForDate(const char *date, const char *fromTime, const char *toTime) {
  if (!mounted || !nameLooksSafe(date, 15)) {
    return 0;
  }
  char dirPath[32];
  snprintf(dirPath, sizeof(dirPath), DCIM_DIR "/%s", date);
  File dir = SD_MMC.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    return 0;
  }
  int count = 0;
  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory() && nameIsJpeg(entry.name()) && passesTimeFilter(entry.name(), fromTime, toTime)) {
      count++;
    }
    entry = dir.openNextFile();
  }
  dir.close();
  return count;
}

bool sdStoragePhotosJson(const char *date, int page, int per, const char *fromTime, const char *toTime, char *out, size_t outSize) {
  if (!mounted || !out || outSize < 64 || !nameLooksSafe(date, 15)) {
    return false;
  }
  if (per < 1) per = 10;
  if (per > 500) per = 500;
  if (page < 0) page = 0;

  char dirPath[32];
  snprintf(dirPath, sizeof(dirPath), DCIM_DIR "/%s", date);

  // Pass 1: count files matching the filter. Pass 2: emit the requested
  // window in directory (chronological) order; the client shows newest-first.
  int total = 0;
  File dir = SD_MMC.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    snprintf(out, outSize, "{\"date\":\"%s\",\"total\":0,\"page\":0,\"per\":%d,\"files\":[]}", date, per);
    return true;
  }
  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory() && nameIsJpeg(entry.name()) && passesTimeFilter(entry.name(), fromTime, toTime)) {
      total++;
    }
    entry = dir.openNextFile();
  }
  dir.close();

  int end = total - page * per;    // exclusive, counted from oldest
  int start = end - per;
  if (start < 0) start = 0;
  if (end < 0) end = 0;

  size_t pos = 0;
  pos += snprintf(out + pos, outSize - pos, "{\"date\":\"%s\",\"total\":%d,\"page\":%d,\"per\":%d,\"files\":[", date, total, page, per);

  dir = SD_MMC.open(dirPath);
  if (dir) {
    int idx = 0;
    bool first = true;
    entry = dir.openNextFile();
    while (entry && idx < end) {
      if (!entry.isDirectory() && nameIsJpeg(entry.name()) && passesTimeFilter(entry.name(), fromTime, toTime)) {
        if (idx >= start) {
          int n = snprintf(out + pos, outSize - pos, "%s{\"n\":\"%s\",\"s\":%u}", first ? "" : ",", entry.name(), (unsigned)entry.size());
          if (n < 0 || (size_t)n >= outSize - pos - 4) {
            break;
          }
          pos += n;
          first = false;
        }
        idx++;
      }
      entry = dir.openNextFile();
    }
    dir.close();
  }

  pos += snprintf(out + pos, outSize - pos, "]}");
  return true;
}

File sdStorageOpenPhoto(const char *date, const char *name) {
  if (!mounted || !nameLooksSafe(date, 15) || !nameLooksSafe(name, 40) || !nameIsJpeg(name)) {
    return File();
  }
  char path[80];
  snprintf(path, sizeof(path), DCIM_DIR "/%s/%s", date, name);
  return SD_MMC.open(path, FILE_READ);
}

static bool dirIsEmpty(const char *path) {
  File dir = SD_MMC.open(path);
  if (!dir || !dir.isDirectory()) {
    return false;
  }
  File entry = dir.openNextFile();
  bool empty = !entry;
  dir.close();
  return empty;
}

bool sdStorageDeletePhoto(const char *date, const char *name) {
  if (!mounted || erasing || !nameLooksSafe(date, 15) || !nameLooksSafe(name, 40) || !nameIsJpeg(name)) {
    return false;
  }
  if (!sdStorageLock(3000)) {
    return false;
  }

  char path[80];
  snprintf(path, sizeof(path), DCIM_DIR "/%s/%s", date, name);
  bool ok = SD_MMC.remove(path);
  if (ok) {
    if (photoCount > 0) {
      photoCount--;
    }
    if (strcmp(date, UNSORTED_NAME) != 0) {
      char dirPath[32];
      snprintf(dirPath, sizeof(dirPath), DCIM_DIR "/%s", date);
      if (dirIsEmpty(dirPath)) {
        SD_MMC.rmdir(dirPath);
      }
    }
  }
  sdStorageUnlock();
  return ok;
}

// Deletes every JPEG under /DCIM, folder by folder. Sensor and analysis
// history have their own erase controls, so clearing photos must not silently
// remove those records too. Takes the mutex per file (not for the whole run)
// and yields regularly so the web server and status polling stay alive.
static void eraseAllTask(void *pvParameters) {
  File root = SD_MMC.open(DCIM_DIR);
  if (root && root.isDirectory()) {
    char dirs[64][16];
    int dirCount = 0;
    File entry = root.openNextFile();
    while (entry && dirCount < 64) {
      if (entry.isDirectory()) {
        snprintf(dirs[dirCount], sizeof(dirs[0]), "%s", entry.name());
        dirCount++;
      }
      entry = root.openNextFile();
    }
    root.close();

    for (int i = 0; i < dirCount; i++) {
      char dirPath[32];
      snprintf(dirPath, sizeof(dirPath), DCIM_DIR "/%s", dirs[i]);
      File dir = SD_MMC.open(dirPath);
      if (dir) {
        int sinceYield = 0;
        File f = dir.openNextFile();
        while (f) {
          char path[80];
          bool isPhoto = !f.isDirectory() && nameIsJpeg(f.name());
          if (isPhoto) {
            snprintf(path, sizeof(path), "%s/%s", dirPath, f.name());
          }
          f.close();
          f = dir.openNextFile();
          if (isPhoto) {
            if (sdStorageLock(3000)) {
              SD_MMC.remove(path);
              if (photoCount > 0) {
                photoCount--;
              }
              sdStorageUnlock();
            }
            if (++sinceYield >= 20) {
              sinceYield = 0;
              vTaskDelay(pdMS_TO_TICKS(10));
            }
          }
        }
        dir.close();
      }
      SD_MMC.rmdir(dirPath);
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  } else if (root) {
    root.close();
  }

  SD_MMC.mkdir(DCIM_DIR "/" UNSORTED_NAME);
  photoCount = 0;
  unsortedIndex = 1;
  erasing = false;
  deviceLog("[sd] erase all finished");
  vTaskDelete(NULL);
}

bool sdStorageEraseAllStart() {
  if (!mounted || erasing) {
    return false;
  }
  erasing = true;
  if (xTaskCreatePinnedToCore(eraseAllTask, "sdErase", 4096, NULL, 1, NULL, 1) != pdPASS) {
    erasing = false;
    return false;
  }
  deviceLog("[sd] erase all started");
  return true;
}

bool sdStorageIsErasing() {
  return erasing;
}
