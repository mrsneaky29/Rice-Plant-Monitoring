#ifndef SD_STORAGE_H
#define SD_STORAGE_H

#include <stdint.h>
#include <stddef.h>
#include <FS.h>

bool sdStorageBegin();

// Shared lock for ALL direct SD_MMC access, across every module that
// touches the card (this one, sensor_storage, water_level_storage,
// device_log). The underlying SD_MMC/SPI driver isn't safe for concurrent
// access from independent mutexes even when each one is only guarding its
// own file -- two unrelated writes (e.g. a scheduled photo save and a
// device-log line) racing each other on the same physical bus can hang or
// corrupt the card. Every module that opens/writes SD_MMC files directly
// must hold this around that access; sdStorageBegin() creates it.
bool sdStorageLock(uint32_t timeoutMs);
void sdStorageUnlock();

bool sdStorageIsMounted();
uint64_t sdStorageFreeBytes();
uint64_t sdStorageTotalBytes();
bool sdStorageIsFull();
uint32_t sdStoragePhotoCount();

// Saves into /DCIM/<YYYY-MM-DD>/IMG_<HHMMSS>.jpg (or /DCIM/UNSORTED/IMG_<n>.jpg
// when the clock is not set). outFilename receives "<date>/<name>".
bool sdStorageSavePhoto(const uint8_t *data, size_t len, char *outFilename, size_t outFilenameSize);
// New captures publish an .upload intent before the completed JPEG is renamed
// into place. A .cloud acknowledgement stops retries without deleting originals.
bool sdStorageFindNextUnuploadedPhoto(char *date, size_t dateSize, char *filename, size_t filenameSize);
bool sdStorageMarkPhotoUploaded(const char *date, const char *filename);
bool sdStorageGetCloudCommand(const char *id, String &json);
bool sdStorageSetCloudCommand(const char *id, const char *json);

// Gallery: JSON builders for the web API. Both return false when the SD
// card is not mounted or the buffer is too small.
bool sdStorageDatesJson(char *out, size_t outSize);

// fromTime/toTime are optional "HHMMSS" strings (pass "" to skip) that
// filter to photos whose capture time falls within [fromTime, toTime].
// Only applies to photos named with the full timestamp scheme; legacy/
// UNSORTED entries without an embedded time always pass the filter.
bool sdStoragePhotosJson(const char *date, int page, int per, const char *fromTime, const char *toTime, char *out, size_t outSize);

// How many files in `date` would match the time filter — used by the
// caller to size the response buffer before the real call.
int sdStoragePhotoCountForDate(const char *date, const char *fromTime, const char *toTime);

// Opens one photo for chunked serving. Returns a closed File on bad input.
File sdStorageOpenPhoto(const char *date, const char *name);

// Deletes one photo; removes the date folder too once it is empty.
bool sdStorageDeletePhoto(const char *date, const char *name);

// Kicks off deletion of every photo in a background task so HTTP stays
// responsive. Progress is visible via sdStorageIsErasing()/photo count.
bool sdStorageEraseAllStart();
bool sdStorageIsErasing();

#endif  // SD_STORAGE_H
