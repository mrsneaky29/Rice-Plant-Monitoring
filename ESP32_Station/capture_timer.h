#ifndef CAPTURE_TIMER_H
#define CAPTURE_TIMER_H

#include <stdint.h>

void captureTimerBegin();
void captureTimerSetEnabled(bool enabled);
void captureTimerSetIntervalSeconds(uint32_t seconds);
bool captureTimerIsEnabled();
uint32_t captureTimerIntervalSeconds();

// Restricts scheduled captures to a daily time window [startMin, endMin)
// in minutes-since-midnight (0-1439), local time. A window that wraps past
// midnight (start > end) is supported, e.g. 22:00-06:00. When disabled,
// captures run all day. If the window is enabled but the device clock
// isn't set yet, captures are held back until it syncs.
void captureTimerSetWindow(bool enabled, uint16_t startMin, uint16_t endMin);
bool captureTimerWindowEnabled();
uint16_t captureTimerWindowStart();
uint16_t captureTimerWindowEnd();

// True right now: window disabled (always active), or enabled and the
// current local time falls inside it.
bool captureTimerInWindow();

#endif  // CAPTURE_TIMER_H
