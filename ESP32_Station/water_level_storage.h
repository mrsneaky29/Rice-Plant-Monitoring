#ifndef WATER_LEVEL_STORAGE_H
#define WATER_LEVEL_STORAGE_H

#include <stddef.h>

// Stores the complete AI result in /DCIM/<date>/plant_analysis.csv.
// NAN waterLevelCm and a negative tillerCount represent unreadable values.
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
);

// JSON rows contain: t, cm, tillers, stage, wconf, tconf, gconf, photo, notes.
// Legacy water_level.csv records from the water-only firmware remain visible
// here too, mapped from millimetres to centimetres with the other fields null.
bool waterLevelStorageReadingsJson(const char *date, int page, int per, const char *fromTime, const char *toTime, char *out, size_t outSize);

// Deletes both current plant_analysis.csv and legacy water_level.csv from every
// date folder without touching photos.
int waterLevelStorageEraseAll();

#endif  // WATER_LEVEL_STORAGE_H
