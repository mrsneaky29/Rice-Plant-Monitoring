#include "water_level_storage.h"
#include <Arduino.h>
#include <limits.h>
#include "FS.h"
#include "SD_MMC.h"
#include "sd_storage.h"
#include "time_keeper.h"

#define DCIM_DIR "/DCIM"
#define ANALYSIS_FILE_NAME "plant_analysis.csv"
#define LEGACY_ANALYSIS_FILE_NAME "water_level.csv"

static bool nameLooksSafe(const char *s, size_t maxLen) {
  if (!s || !s[0] || strlen(s) > maxLen) return false;
  return !strstr(s, "..") && !strchr(s, '/') && !strchr(s, '\\');
}

static void csvSafeCopy(char *out, size_t outSize, const char *value) {
  if (!out || outSize == 0) return;
  size_t pos = 0;
  if (value) {
    for (size_t i = 0; value[i] && pos + 1 < outSize; i++) {
      char c = value[i];
      out[pos++] = (c == ',' || c == '\r' || c == '\n') ? ' ' : c;
    }
  }
  out[pos] = '\0';
}

static String jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '"' || c == '\\') {
      escaped += '\\';
      escaped += c;
    } else if (c == '\n' || c == '\r' || c == '\t') {
      escaped += ' ';
    } else if ((uint8_t)c >= 0x20) {
      escaped += c;
    }
  }
  return escaped;
}

static bool parseFloatField(const String &field, float *out) {
  if (!out || field.length() == 0) return false;
  char *end = NULL;
  float value = strtof(field.c_str(), &end);
  return end && *end == '\0' && isfinite(value) && ((*out = value), true);
}

static bool parseIntField(const String &field, int *out) {
  if (!out || field.length() == 0) return false;
  char *end = NULL;
  long value = strtol(field.c_str(), &end, 10);
  if (!end || *end != '\0' || value < INT32_MIN || value > INT32_MAX) return false;
  *out = (int)value;
  return true;
}

bool waterLevelStorageAppend(
  const char *sourceDate,
  float waterLevelCm,
  int tillerCount,
  const char *growthStage,
  float waterConfidence,
  float tillerConfidence,
  float growthConfidence,
  const char *photoName,
  const char *notes
) {
  if (!sdStorageIsMounted() || sdStorageIsFull() || !nameLooksSafe(sourceDate, 15)) return false;

  char date[16], hhmmss[8];
  snprintf(date, sizeof(date), "%s", sourceDate);
  // Normal photo names are YYYY-MM-DD__HH-MM-SS-mmm.jpg. Use that capture
  // time so a response arriving after midnight still joins the correct photo.
  if (photoName && strlen(photoName) >= 20 && photoName[10] == '_' && photoName[11] == '_' && photoName[14] == '-' && photoName[17] == '-') {
    snprintf(hhmmss, sizeof(hhmmss), "%c%c%c%c%c%c", photoName[12], photoName[13], photoName[15], photoName[16], photoName[18], photoName[19]);
  } else if (!timeKeeperLocalTimeCompact(hhmmss, sizeof(hhmmss))) {
    return false;
  }
  if (!sdStorageLock(3000)) return false;

  char dirPath[32];
  snprintf(dirPath, sizeof(dirPath), DCIM_DIR "/%s", date);
  if (!SD_MMC.exists(dirPath)) SD_MMC.mkdir(dirPath);

  char path[64];
  snprintf(path, sizeof(path), "%s/%s", dirPath, ANALYSIS_FILE_NAME);

  char safeStage[48], safePhoto[48], safeNotes[128];
  csvSafeCopy(safeStage, sizeof(safeStage), growthStage);
  csvSafeCopy(safePhoto, sizeof(safePhoto), photoName);
  csvSafeCopy(safeNotes, sizeof(safeNotes), notes);

  char cmField[20], tillerField[16];
  if (isnan(waterLevelCm)) cmField[0] = '\0';
  else snprintf(cmField, sizeof(cmField), "%.1f", waterLevelCm);
  if (tillerCount < 0) tillerField[0] = '\0';
  else snprintf(tillerField, sizeof(tillerField), "%d", tillerCount);

  bool ok = false;
  File f = SD_MMC.open(path, FILE_APPEND);
  if (f) {
    char line[384];
    int n = snprintf(
      line, sizeof(line), "%s,%s,%s,%s,%.3f,%.3f,%.3f,%s,%s\n",
      hhmmss, cmField, tillerField, safeStage,
      waterConfidence, tillerConfidence, growthConfidence, safePhoto, safeNotes
    );
    ok = n > 0 && (size_t)n < sizeof(line) && f.write((const uint8_t *)line, n) == (size_t)n;
    f.close();
  }

  sdStorageUnlock();
  return ok;
}

static bool passesTimeFilter(const char *hhmmss, const char *fromTime, const char *toTime) {
  if (fromTime && fromTime[0] && strcmp(hhmmss, fromTime) < 0) return false;
  if (toTime && toTime[0] && strcmp(hhmmss, toTime) > 0) return false;
  return true;
}

static bool splitCsvLine(const String &line, String fields[9]) {
  int start = 0;
  for (int i = 0; i < 8; i++) {
    int comma = line.indexOf(',', start);
    if (comma < 0) return false;
    fields[i] = line.substring(start, comma);
    start = comma + 1;
  }
  fields[8] = line.substring(start);
  return fields[0].length() >= 6;
}

static bool splitLegacyCsvLine(const String &line, String fields[4]) {
  int start = 0;
  for (int i = 0; i < 3; i++) {
    int comma = line.indexOf(',', start);
    if (comma < 0) return false;
    fields[i] = line.substring(start, comma);
    start = comma + 1;
  }
  fields[3] = line.substring(start);
  return fields[0].length() >= 6;
}

static int countCurrentRows(const char *path, const char *fromTime, const char *toTime) {
  int total = 0;
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    String fields[9];
    if (splitCsvLine(line, fields) && passesTimeFilter(fields[0].c_str(), fromTime, toTime)) total++;
  }
  f.close();
  return total;
}

static int countLegacyRows(const char *path, const char *fromTime, const char *toTime) {
  int total = 0;
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    String fields[4];
    if (splitLegacyCsvLine(line, fields) && passesTimeFilter(fields[0].c_str(), fromTime, toTime)) total++;
  }
  f.close();
  return total;
}

static bool appendCurrentRow(
  const String fields[9], char *out, size_t outSize, size_t *pos, bool *first
) {
  float cmValue, waterConfidence, tillerConfidence, growthConfidence;
  int tillersValue;
  char cmField[24] = "null";
  char tillerField[16] = "null";
  char waterConfidenceField[16] = "0";
  char tillerConfidenceField[16] = "0";
  char growthConfidenceField[16] = "0";
  if (parseFloatField(fields[1], &cmValue) && cmValue >= 0) {
    snprintf(cmField, sizeof(cmField), "%.3f", cmValue);
  }
  if (parseIntField(fields[2], &tillersValue) && tillersValue >= 0) {
    snprintf(tillerField, sizeof(tillerField), "%d", tillersValue);
  }
  if (parseFloatField(fields[4], &waterConfidence) && waterConfidence >= 0 && waterConfidence <= 1) {
    snprintf(waterConfidenceField, sizeof(waterConfidenceField), "%.3f", waterConfidence);
  }
  if (parseFloatField(fields[5], &tillerConfidence) && tillerConfidence >= 0 && tillerConfidence <= 1) {
    snprintf(tillerConfidenceField, sizeof(tillerConfidenceField), "%.3f", tillerConfidence);
  }
  if (parseFloatField(fields[6], &growthConfidence) && growthConfidence >= 0 && growthConfidence <= 1) {
    snprintf(growthConfidenceField, sizeof(growthConfidenceField), "%.3f", growthConfidence);
  }

  String stage = jsonEscape(fields[3]);
  String photo = jsonEscape(fields[7]);
  String notes = jsonEscape(fields[8]);
  char timeDisplay[10];
  snprintf(timeDisplay, sizeof(timeDisplay), "%c%c:%c%c:%c%c", fields[0][0], fields[0][1], fields[0][2], fields[0][3], fields[0][4], fields[0][5]);
  int n = snprintf(
    out + *pos, outSize - *pos,
    "%s{\"t\":\"%s\",\"cm\":%s,\"tillers\":%s,\"stage\":\"%s\",\"wconf\":%s,\"tconf\":%s,\"gconf\":%s,\"photo\":\"%s\",\"notes\":\"%s\"}",
    *first ? "" : ",", timeDisplay, cmField, tillerField, stage.c_str(),
    waterConfidenceField, tillerConfidenceField, growthConfidenceField, photo.c_str(), notes.c_str()
  );
  if (n < 0 || (size_t)n >= outSize - *pos - 4) return false;
  *pos += (size_t)n;
  *first = false;
  return true;
}

static float legacyConfidenceToFloat(const String &confidence) {
  if (confidence.equalsIgnoreCase("high")) return 0.9f;
  if (confidence.equalsIgnoreCase("medium")) return 0.6f;
  if (confidence.equalsIgnoreCase("low")) return 0.3f;
  return 0.0f;
}

static bool appendLegacyRow(
  const String fields[4], char *out, size_t outSize, size_t *pos, bool *first
) {
  float mmValue;
  char cmField[24] = "null";
  if (parseFloatField(fields[1], &mmValue) && mmValue >= 0) {
    snprintf(cmField, sizeof(cmField), "%.3f", mmValue / 10.0f);
  }
  char waterConfidenceField[16];
  snprintf(waterConfidenceField, sizeof(waterConfidenceField), "%.3f", legacyConfidenceToFloat(fields[2]));
  String photo = jsonEscape(fields[3]);
  char timeDisplay[10];
  snprintf(timeDisplay, sizeof(timeDisplay), "%c%c:%c%c:%c%c", fields[0][0], fields[0][1], fields[0][2], fields[0][3], fields[0][4], fields[0][5]);
  int n = snprintf(
    out + *pos, outSize - *pos,
    "%s{\"t\":\"%s\",\"cm\":%s,\"tillers\":null,\"stage\":\"\",\"wconf\":%s,\"tconf\":0,\"gconf\":0,\"photo\":\"%s\",\"notes\":\"Imported legacy water-level reading\"}",
    *first ? "" : ",", timeDisplay, cmField, waterConfidenceField, photo.c_str()
  );
  if (n < 0 || (size_t)n >= outSize - *pos - 4) return false;
  *pos += (size_t)n;
  *first = false;
  return true;
}

static bool appendCurrentRows(
  const char *path, const char *fromTime, const char *toTime, int start, int end,
  int *matchedIndex, char *out, size_t outSize, size_t *pos, bool *first
) {
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    String fields[9];
    if (!splitCsvLine(line, fields) || !passesTimeFilter(fields[0].c_str(), fromTime, toTime)) continue;
    if (*matchedIndex >= start && *matchedIndex < end && !appendCurrentRow(fields, out, outSize, pos, first)) {
      f.close();
      return false;
    }
    (*matchedIndex)++;
  }
  f.close();
  return true;
}

static bool appendLegacyRows(
  const char *path, const char *fromTime, const char *toTime, int start, int end,
  int *matchedIndex, char *out, size_t outSize, size_t *pos, bool *first
) {
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    String fields[4];
    if (!splitLegacyCsvLine(line, fields) || !passesTimeFilter(fields[0].c_str(), fromTime, toTime)) continue;
    if (*matchedIndex >= start && *matchedIndex < end && !appendLegacyRow(fields, out, outSize, pos, first)) {
      f.close();
      return false;
    }
    (*matchedIndex)++;
  }
  f.close();
  return true;
}

bool waterLevelStorageReadingsJson(const char *date, int page, int per, const char *fromTime, const char *toTime, char *out, size_t outSize) {
  if (!out || outSize < 128 || !nameLooksSafe(date, 15)) return false;
  if (per < 1) per = 20;
  if (per > 100) per = 100;
  if (page < 0) page = 0;

  char currentPath[64], legacyPath[64];
  snprintf(currentPath, sizeof(currentPath), DCIM_DIR "/%s/%s", date, ANALYSIS_FILE_NAME);
  snprintf(legacyPath, sizeof(legacyPath), DCIM_DIR "/%s/%s", date, LEGACY_ANALYSIS_FILE_NAME);
  if (!sdStorageIsMounted()) {
    snprintf(out, outSize, "{\"date\":\"%s\",\"total\":0,\"page\":0,\"per\":%d,\"rows\":[]}", date, per);
    return true;
  }
  if (!sdStorageLock(5000)) return false;

  int total = countCurrentRows(currentPath, fromTime, toTime) + countLegacyRows(legacyPath, fromTime, toTime);
  int end = total - page * per;
  int start = end - per;
  if (start < 0) start = 0;
  if (end < 0) end = 0;

  size_t pos = 0;
  int headerLength = snprintf(out, outSize, "{\"date\":\"%s\",\"total\":%d,\"page\":%d,\"per\":%d,\"rows\":[", date, total, page, per);
  if (headerLength < 0 || (size_t)headerLength >= outSize - 4) {
    sdStorageUnlock();
    return false;
  }
  pos = (size_t)headerLength;

  int matchedIndex = 0;
  bool first = true;
  bool ok = appendCurrentRows(currentPath, fromTime, toTime, start, end, &matchedIndex, out, outSize, &pos, &first) &&
            appendLegacyRows(legacyPath, fromTime, toTime, start, end, &matchedIndex, out, outSize, &pos, &first);
  if (ok && pos + 3 < outSize) {
    snprintf(out + pos, outSize - pos, "]}");
  } else {
    snprintf(out, outSize, "{\"error\":\"response_too_large\"}");
    ok = false;
  }

  sdStorageUnlock();
  return ok;
}

int waterLevelStorageEraseAll() {
  if (!sdStorageIsMounted() || !sdStorageLock(5000)) return 0;

  int removed = 0;
  File root = SD_MMC.open(DCIM_DIR);
  if (root && root.isDirectory()) {
    File entry = root.openNextFile();
    while (entry) {
      if (entry.isDirectory()) {
        char currentPath[64], legacyPath[64];
        snprintf(currentPath, sizeof(currentPath), "%s/%s/%s", DCIM_DIR, entry.name(), ANALYSIS_FILE_NAME);
        snprintf(legacyPath, sizeof(legacyPath), "%s/%s/%s", DCIM_DIR, entry.name(), LEGACY_ANALYSIS_FILE_NAME);
        if (SD_MMC.exists(currentPath) && SD_MMC.remove(currentPath)) removed++;
        if (SD_MMC.exists(legacyPath) && SD_MMC.remove(legacyPath)) removed++;
      }
      entry = root.openNextFile();
    }
    root.close();
  }

  sdStorageUnlock();
  return removed;
}
