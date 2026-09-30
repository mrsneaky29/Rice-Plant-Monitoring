#ifndef WATER_LEVEL_H
#define WATER_LEVEL_H

#include <stdint.h>
#include <stddef.h>

// Starts the background rice-plant analysis task. Call once from setup().
void waterLevelBegin();

void waterLevelSetVisionEnabled(bool enabled);
bool waterLevelVisionEnabled();

enum WaterLevelQueueResult {
  WATER_LEVEL_QUEUE_QUEUED,
  WATER_LEVEL_QUEUE_ALREADY_PENDING,
  WATER_LEVEL_QUEUE_UNAVAILABLE,
  WATER_LEVEL_QUEUE_FULL,
  WATER_LEVEL_QUEUE_INVALID,
};

// Queues an already-saved photo (any capture path -- manual, scheduled,
// live-view snap) for water-level, tiller-count and growth-stage analysis.
// Non-blocking: returns immediately, does nothing if vision is off, and
// silently drops the request if the queue is momentarily full (a missed
// analysis on one photo isn't worth blocking the capture path over).
void waterLevelEnqueueAnalysis(const char *date, const char *name);

// Queues one user-selected gallery photo regardless of whether automatic
// analysis is enabled. The caller should verify that an API key is present
// and that the photo exists before calling this function.
WaterLevelQueueResult waterLevelEnqueueManualAnalysis(const char *date, const char *name);

// Cancels queued/in-flight analysis before an OTA update.
void waterLevelCancelAll();

#endif  // WATER_LEVEL_H
