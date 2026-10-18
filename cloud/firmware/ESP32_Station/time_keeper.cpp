#include "time_keeper.h"
#include <Arduino.h>
#include <time.h>
#include <sys/time.h>
#include "device_log.h"

// Any epoch before this means the clock was never set (2020-09-13).
#define MIN_VALID_EPOCH 1600000000UL

// Default to IST (+5:30) until a browser reports its real offset.
static volatile int tzOffsetMin = 330;

void timeKeeperBegin() {
  // UTC internally; local offset is applied only when formatting.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

void timeKeeperSetFromEpoch(uint32_t epochUtc, int tzOffsetMinutes) {
  if (tzOffsetMinutes >= -840 && tzOffsetMinutes <= 840) {
    tzOffsetMin = tzOffsetMinutes;
  }
  if (epochUtc < MIN_VALID_EPOCH) {
    return;
  }
  time_t now = time(NULL);
  long drift = (long)now - (long)epochUtc;
  if (now < (time_t)MIN_VALID_EPOCH || drift > 10 || drift < -10) {
    struct timeval tv = { .tv_sec = (time_t)epochUtc, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    deviceLog("[time] clock set from web page, epoch %lu tz %+d min", (unsigned long)epochUtc, tzOffsetMin);
  }
}

bool timeKeeperIsSet() {
  return time(NULL) >= (time_t)MIN_VALID_EPOCH;
}

int timeKeeperTzOffsetMinutes() {
  return tzOffsetMin;
}

static bool localTm(struct tm *out) {
  time_t now = time(NULL);
  if (now < (time_t)MIN_VALID_EPOCH) {
    return false;
  }
  time_t local = now + (time_t)tzOffsetMin * 60;
  gmtime_r(&local, out);
  return true;
}

bool timeKeeperLocalDate(char *out, size_t outSize) {
  struct tm t;
  if (!localTm(&t)) {
    if (outSize) out[0] = '\0';
    return false;
  }
  snprintf(out, outSize, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return true;
}

bool timeKeeperLocalTimeCompact(char *out, size_t outSize) {
  struct tm t;
  if (!localTm(&t)) {
    if (outSize) out[0] = '\0';
    return false;
  }
  snprintf(out, outSize, "%02d%02d%02d", t.tm_hour, t.tm_min, t.tm_sec);
  return true;
}

bool timeKeeperLocalDateTime(char *out, size_t outSize) {
  struct tm t;
  if (!localTm(&t)) {
    if (outSize) out[0] = '\0';
    return false;
  }
  snprintf(out, outSize, "%04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  return true;
}

int timeKeeperLocalMinuteOfDay() {
  struct tm t;
  if (!localTm(&t)) {
    return -1;
  }
  return t.tm_hour * 60 + t.tm_min;
}

bool timeKeeperLocalStampMs(char *out, size_t outSize) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  if (tv.tv_sec < (time_t)MIN_VALID_EPOCH) {
    if (outSize) out[0] = '\0';
    return false;
  }
  time_t local = tv.tv_sec + (time_t)tzOffsetMin * 60;
  struct tm t;
  gmtime_r(&local, &t);
  int ms = tv.tv_usec / 1000;
  // Colons aren't valid in filenames on FAT/Windows, so date and time are
  // joined with "__" and each field uses "-" internally: 2026-08-15__22-50-32-123
  snprintf(out, outSize, "%04d-%02d-%02d__%02d-%02d-%02d-%03d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, ms);
  return true;
}
