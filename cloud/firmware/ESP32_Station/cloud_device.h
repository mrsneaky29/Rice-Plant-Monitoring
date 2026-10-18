#ifndef CLOUD_DEVICE_H
#define CLOUD_DEVICE_H

#include <stdint.h>

void cloudDeviceBegin();
bool cloudDeviceConfigured();
bool cloudDeviceSleepEnabled();
bool cloudDeviceStayAwake();
void cloudDeviceSetSleepEnabled(bool enabled);
void cloudDeviceRequestStayAwake(bool enabled);
void cloudDeviceSetOtaActive(bool active);
void cloudDeviceLoop();
void cloudDeviceScheduledWake();
bool cloudDeviceTimerWakePending();
bool cloudDeviceCycleActive();
bool cloudDeviceScheduledPending();
uint32_t cloudDeviceMaintenanceRemainingSeconds();
uint32_t cloudDeviceUploadErrorCount();
const char *cloudDeviceLastUploadError();

#endif
