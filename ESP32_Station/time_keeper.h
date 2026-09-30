#ifndef TIME_KEEPER_H
#define TIME_KEEPER_H

#include <stdint.h>
#include <stddef.h>

// Starts background NTP sync (call once after WiFi connects).
void timeKeeperBegin();

// Sets the clock from a browser-supplied UTC epoch + timezone offset
// (minutes east of UTC). Only overwrites the clock if it is unset or
// drifted; the timezone offset is always updated.
void timeKeeperSetFromEpoch(uint32_t epochUtc, int tzOffsetMinutes);

bool timeKeeperIsSet();
int timeKeeperTzOffsetMinutes();

// All formatters return false (and write "") when the clock is not set.
bool timeKeeperLocalDate(char *out, size_t outSize);         // YYYY-MM-DD
bool timeKeeperLocalTimeCompact(char *out, size_t outSize);  // HHMMSS
bool timeKeeperLocalDateTime(char *out, size_t outSize);     // YYYY-MM-DD HH:MM:SS
bool timeKeeperLocalStampMs(char *out, size_t outSize);      // YYYY-MM-DD__HH-MM-SS-mmm

// Minutes since local midnight (0-1439), or -1 if the clock is not set.
int timeKeeperLocalMinuteOfDay();

#endif  // TIME_KEEPER_H
