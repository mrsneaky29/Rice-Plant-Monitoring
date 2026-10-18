#ifndef CAPTURE_TIMER_H
#define CAPTURE_TIMER_H

#include <stdint.h>

void captureTimerBegin();
void captureTimerSetEnabled(bool enabled);
void captureTimerSetIntervalSeconds(uint32_t seconds);
bool captureTimerIsEnabled();
uint32_t captureTimerIntervalSeconds();
uint32_t captureTimerSecondsUntilNext();

// Restricts scheduled captures to a daily time window [startMin, endMin)
// in minutes-since-midnight (0-1439), local time. A window that wraps past
// midnight (start > end) is supported, e.g. 22:00-06:00. When disabled,
// captures run all day. If the window is enabled but the device clock
// isn't set yet, captures are held back until it syncs.
void captureTimerSetWindow(bool enabled, uint16_t startMin, uint16_t endMin);
void captureTimerRestoreSchedule(bool enabled, uint32_t intervalSeconds,
                                 bool windowEnabled, uint16_t startMin,
                                 uint16_t endMin);
// Atomically consumes an overdue schedule and advances its RTC deadline before
// capture begins. Returns true only once per scheduled event.
bool captureTimerConsumeDue();
bool captureTimerWindowEnabled();
uint16_t captureTimerWindowStart();
uint16_t captureTimerWindowEnd();

// True right now: window disabled (always active), or enabled and the
// current local time falls inside it.
bool captureTimerInWindow();

#endif  // CAPTURE_TIMER_H
