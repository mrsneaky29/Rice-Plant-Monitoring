#include "sd_storage.h"
#include "cloud_run_mode.h"
#include "cloud_upload_policy.h"
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
    } else if (nameIsJpeg(entry.name())) {
      photoCount++;  // Count legacy flat originals without relocating them.
    }
    entry = root.openNextFile();
  }
  root.close();
}

bool sdStorageBegin() {
#if RICE_CLOUD_STATUS_ONLY
  // Connection diagnosis must not touch the card, including FAT mount repair,
  // legacy-photo migration, directory creation, capture, or acknowledgement.
  deviceLog("[sd] status-only diagnostics: SD card left unmounted and untouched");
  return false;
#else
  if (!sdMutex) {
    sdMutex = xSemaphoreCreateMutex();
  }

  mounted = SD_MMC.begin("/sdcard", true, false);
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

  // Never move or rename pre-existing originals automatically. A reader can
  // retrieve legacy flat files in their original locations.
  scanExistingPhotos();
  deviceLog("SD Card mounted. %u existing photos", (unsigned)photoCount);
  return true;
#endif
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
  if (!mounted || !sdStorageLock(3000)) return 0;
  uint64_t total = SD_MMC.totalBytes();
  uint64_t used = SD_MMC.usedBytes();
  sdStorageUnlock();
  return total > used ? total - used : 0;
}

uint64_t sdStorageTotalBytes() {
  if (!mounted || !sdStorageLock(3000)) return 0;
  uint64_t total = SD_MMC.totalBytes();
  sdStorageUnlock();
  return total;
}

bool sdStorageIsFull() {
  if (!mounted) {
    return true;
  }
  // Uploads may hold the card mutex longer than a status probe. A busy card
  // is not a full card: treating lock contention as zero free bytes would
  // incorrectly disable the capture timer while an upload is in progress.
  if (!sdStorageLock(3000)) return false;
  uint64_t total = SD_MMC.totalBytes();
  uint64_t used = SD_MMC.usedBytes();
  sdStorageUnlock();
  return total <= used || total - used < MIN_FREE_BYTES;
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
    do {
      snprintf(name, sizeof(name), "IMG_%05u.jpg", (unsigned)unsortedIndex++);
      snprintf(path, sizeof(path), "%s/%s", dirPath, name);
    } while (SD_MMC.exists(path));
  }

  char temporary[112], pending[112];
  snprintf(temporary, sizeof(temporary), "%s.part", path);
  snprintf(pending, sizeof(pending), "%s.upload", path);
  // A filename is visible as a JPEG only after the complete write is closed.
  // Refuse stale sidecars rather than touching files left by an earlier boot.
  if (SD_MMC.exists(temporary) || SD_MMC.exists(pending)) {
    sdStorageUnlock();
    return false;
  }
  File file = SD_MMC.open(temporary, FILE_WRITE);
  if (!file) {
    deviceLog("Failed to open %s for writing", path);
    sdStorageUnlock();
    return false;
  }

  size_t written = file.write(data, len);
  file.close();

  if (written != len) {
    deviceLog("Short write on %s (%u of %u bytes)", path, (unsigned)written, (unsigned)len);
    SD_MMC.remove(temporary);
    sdStorageUnlock();
    return false;
  }

  // Commit upload intent before publishing the JPEG. A reset at either step
  // cannot make an old original eligible or expose an incomplete new JPEG.
  File intent = SD_MMC.open(pending, FILE_WRITE);
  if (!intent) {
    sdStorageUnlock();
    return false;  // retain the completed .part file for manual recovery
  }
  intent.close();
  if (!SD_MMC.rename(temporary, path)) {
    sdStorageUnlock();
    return false;  // retain both new files; never delete an existing original
  }

  if (outFilename && outFilenameSize > 0) {
    snprintf(outFilename, outFilenameSize, "%s/%s", date, name);
  }

  photoCount++;
  sdStorageUnlock();
  return true;
}

static const char *entryBaseName(const char *name) {
  const char *slash = strrchr(name, '/');
  return slash ? slash + 1 : name;
}

static bool safePhotoPart(const char *part, bool datePart) {
  if (!part || !part[0] || strchr(part, '/') || strchr(part, '\\') || strstr(part, "..")) return false;
  if (datePart && strcmp(part, UNSORTED_NAME) == 0) return true;
  for (const char *p = part; *p; p++) {
    if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == '.')) return false;
  }
  return true;
}

bool sdStorageFindNextUnuploadedPhoto(char *date, size_t dateSize, char *filename, size_t filenameSize) {
  if (!mounted || !date || !filename || dateSize < 2 || filenameSize < 2 || !sdStorageLock(3000)) return false;
  bool found = false;
  File root = SD_MMC.open(DCIM_DIR);
  if (root && root.isDirectory()) {
    File entry = root.openNextFile();
    while (entry && !found) {
      if (entry.isDirectory()) {
        char datePart[16];
        snprintf(datePart, sizeof(datePart), "%s", entryBaseName(entry.name()));
        entry.close();

        if (strlen(datePart) < dateSize) {
          char path[64];
          snprintf(path, sizeof(path), DCIM_DIR "/%s", datePart);
          File photos = SD_MMC.open(path);
          if (photos && photos.isDirectory()) {
            File photo = photos.openNextFile();
            while (photo && !found) {
              if (!photo.isDirectory()) {
                const char *base = entryBaseName(photo.name());
                size_t n = strlen(base);
                if (n > 4 && strcasecmp(base + n - 4, ".jpg") == 0 && n < filenameSize) {
                  char marker[112];
                  snprintf(marker, sizeof(marker), "%s/%s.cloud", path, base);
                  char pending[112];
                  snprintf(pending, sizeof(pending), "%s/%s.upload", path, base);
                  if (cloudPhotoShouldUpload(SD_MMC.exists(marker), SD_MMC.exists(pending),
                                             RICE_CLOUD_UPLOAD_EXISTING_PHOTOS != 0)) {
                    snprintf(date, dateSize, "%s", datePart);
                    snprintf(filename, filenameSize, "%s", base);
                    found = true;
                  }
                }
              }
              photo.close();
              if (!found) photo = photos.openNextFile();
            }
            photos.close();
          } else if (photos) {
            photos.close();
          }
        }
      } else {
        entry.close();
      }
      if (!found) entry = root.openNextFile();
    }
    root.close();
  } else if (root) {
    root.close();
  }
  sdStorageUnlock();
  return found;
}

bool sdStorageMarkPhotoUploaded(const char *date, const char *filename) {
  if (!mounted || !safePhotoPart(date, true) || !safePhotoPart(filename, false) || !sdStorageLock(3000)) return false;
  char photo[112], marker[120];
  snprintf(photo, sizeof(photo), DCIM_DIR "/%s/%s", date, filename);
  snprintf(marker, sizeof(marker), "%s.cloud", photo);
  bool ok = SD_MMC.exists(photo);
  if (ok && !SD_MMC.exists(marker)) {
    File ack = SD_MMC.open(marker, FILE_WRITE);
    ok = (bool)ack;
    if (ack) ack.close();
  }
  sdStorageUnlock();
  return ok;
}

static bool safeCloudCommandId(const char *id) {
  if (!id || !id[0] || strlen(id) > 64) return false;
  for (const char *p = id; *p; p++) {
    if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_')) return false;
  }
  return true;
}

bool sdStorageGetCloudCommand(const char *id, String &json) {
  if (!mounted || !safeCloudCommandId(id) || !sdStorageLock(3000)) return false;
  char path[96];
  snprintf(path, sizeof(path), "/CLOUD/CMD_%s.json", id);
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) {
    char recovery[104];
    snprintf(recovery, sizeof(recovery), "/CLOUD/CMD_%s.tmp", id);
    f = SD_MMC.open(recovery, FILE_READ);
    if (!f) {
      snprintf(recovery, sizeof(recovery), "/CLOUD/CMD_%s.bak", id);
      f = SD_MMC.open(recovery, FILE_READ);
    }
  }
  bool ok = (bool)f;
  if (f) { json = f.readString(); f.close(); }
  sdStorageUnlock();
  return ok;
}

bool sdStorageSetCloudCommand(const char *id, const char *json) {
  if (!mounted || !safeCloudCommandId(id) || !json || !sdStorageLock(3000)) return false;
  if (!SD_MMC.exists("/CLOUD")) SD_MMC.mkdir("/CLOUD");
  char path[96], temp[104], backup[104];
  snprintf(path, sizeof(path), "/CLOUD/CMD_%s.json", id);
  snprintf(temp, sizeof(temp), "/CLOUD/CMD_%s.tmp", id);
  snprintf(backup, sizeof(backup), "/CLOUD/CMD_%s.bak", id);
  SD_MMC.remove(temp);
  File f = SD_MMC.open(temp, FILE_WRITE);
  bool ok = (bool)f;
  if (f) {
    size_t n = f.write((const uint8_t *)json, strlen(json));
    f.flush();
    f.close();
    ok = n == strlen(json);
  }
  if (ok) {
    SD_MMC.remove(backup);
    bool hadPrevious = SD_MMC.exists(path);
    if (hadPrevious) ok = SD_MMC.rename(path, backup);
    if (ok) ok = SD_MMC.rename(temp, path);
    if (!ok && hadPrevious && !SD_MMC.exists(path)) SD_MMC.rename(backup, path);
    if (ok) SD_MMC.remove(backup);
  }
  if (!ok) SD_MMC.remove(temp);
  sdStorageUnlock();
  return ok;
}

bool sdStorageDatesJson(char *out, size_t outSize) {
  if (!mounted || !out || outSize < 32 || !sdStorageLock(3000)) {
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
  sdStorageUnlock();
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
  if (!mounted || !nameLooksSafe(date, 15) || !sdStorageLock(3000)) {
    return 0;
  }
  char dirPath[32];
  snprintf(dirPath, sizeof(dirPath), DCIM_DIR "/%s", date);
  File dir = SD_MMC.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    sdStorageUnlock();
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
  sdStorageUnlock();
  return count;
}

bool sdStoragePhotosJson(const char *date, int page, int per, const char *fromTime, const char *toTime, char *out, size_t outSize) {
  if (!mounted || !out || outSize < 64 || !nameLooksSafe(date, 15) || !sdStorageLock(3000)) {
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
    sdStorageUnlock();
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
  sdStorageUnlock();
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
#if RICE_SD_PROTECT_PHOTOS
  return false;
#endif
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
    char marker[96];
    snprintf(marker, sizeof(marker), "%s.cloud", path);
    SD_MMC.remove(marker);
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
// history have their own erase controls. Each bounded batch holds the shared
// SD mutex across directory iteration, unlink, and rmdir operations.
static void eraseAllTask(void *pvParameters) {
  if (!sdStorageLock(5000)) {
    erasing = false;
    vTaskDelete(NULL);
    return;
  }
  File root = SD_MMC.open(DCIM_DIR);
  char dirs[64][16];
  int dirCount = 0;
  if (root && root.isDirectory()) {
    File entry = root.openNextFile();
    while (entry && dirCount < 64) {
      if (entry.isDirectory()) {
        snprintf(dirs[dirCount], sizeof(dirs[0]), "%s", entry.name());
        dirCount++;
      }
      entry = root.openNextFile();
    }
    root.close();
  } else if (root) {
    root.close();
  }
  sdStorageUnlock();

  for (int i = 0; i < dirCount; i++) {
    char dirPath[32];
    snprintf(dirPath, sizeof(dirPath), DCIM_DIR "/%s", dirs[i]);
    bool more = true;
    while (more && sdStorageLock(5000)) {
      File dir = SD_MMC.open(dirPath);
      char paths[20][112];
      int count = 0;
      if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f && count < 20) {
          if (!f.isDirectory() && nameIsJpeg(f.name())) {
            snprintf(paths[count], sizeof(paths[count]), "%s/%s", dirPath, f.name());
            count++;
          }
          f.close();
          f = dir.openNextFile();
        }
        dir.close();
      } else if (dir) {
        dir.close();
      }
      for (int j = 0; j < count; j++) {
        if (SD_MMC.remove(paths[j])) {
          char marker[120];
          snprintf(marker, sizeof(marker), "%s.cloud", paths[j]);
          SD_MMC.remove(marker);
          if (photoCount > 0) photoCount--;
        }
      }
      more = count == 20;
      if (!more) SD_MMC.rmdir(dirPath);
      sdStorageUnlock();
      if (more) vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  if (sdStorageLock(5000)) {
    SD_MMC.mkdir(DCIM_DIR "/" UNSORTED_NAME);
    sdStorageUnlock();
  }
  photoCount = 0;
  unsortedIndex = 1;
  erasing = false;
  deviceLog("[sd] erase all finished");
  vTaskDelete(NULL);
}

bool sdStorageEraseAllStart() {
#if RICE_SD_PROTECT_PHOTOS
  return false;
#endif
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
