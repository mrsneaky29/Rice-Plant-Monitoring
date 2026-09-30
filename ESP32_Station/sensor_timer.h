#ifndef SENSOR_TIMER_H
#define SENSOR_TIMER_H

#include <stdint.h>

void sensorTimerBegin();
void sensorTimerSetEnabled(bool enabled);
void sensorTimerSetIntervalSeconds(uint32_t seconds);
bool sensorTimerIsEnabled();
uint32_t sensorTimerIntervalSeconds();

// Same daily time-window scheme as capture_timer: [startMin, endMin) in
// minutes-since-midnight, local time, wraps past midnight if start > end.
void sensorTimerSetWindow(bool enabled, uint16_t startMin, uint16_t endMin);
bool sensorTimerWindowEnabled();
uint16_t sensorTimerWindowStart();
uint16_t sensorTimerWindowEnd();
bool sensorTimerInWindow();

#endif  // SENSOR_TIMER_H
