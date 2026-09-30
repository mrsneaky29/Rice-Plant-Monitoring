#ifndef SENSOR_STORAGE_H
#define SENSOR_STORAGE_H

#include <stddef.h>

// Appends one reading into /DCIM/<date>/sensors.csv -- the same date
// folders photos live in. Requires the clock to be set (otherwise there's
// no date to file it under); returns false if it couldn't be written.
bool sensorStorageAppend(float tempC, float humidity);
bool sensorStorageAppendDht(float tempC, float humidity);
bool sensorStorageAppendSoil(float volts, float percent);

// fromTime/toTime are optional "HHMMSS" strings (pass "" to skip), same
// scheme as the photo gallery's time filter.
// JSON: {"date":..,"total":N,"page":P,"per":N,"rows":[{"t":"HH:MM:SS","c":21.5,"h":44.0}]}
bool sensorStorageReadingsJson(const char *date, int page, int per, const char *fromTime, const char *toTime, char *out, size_t outSize);

// Deletes sensors.csv from every date folder under /DCIM -- clears all
// DHT history without touching photos or any other date-folder contents.
// Returns the number of files removed.
int sensorStorageEraseAll();

#endif  // SENSOR_STORAGE_H
