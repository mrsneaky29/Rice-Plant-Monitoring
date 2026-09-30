#ifndef CAMERA_SETUP_H
#define CAMERA_SETUP_H

#include <stdbool.h>
#include "esp_camera.h"

// Initializes the camera driver and applies sensor-specific tuning.
// Returns true when esp_camera_init() succeeded and the sensor responds.
bool cameraSetupBegin();

// Human-readable sensor label set during cameraSetupBegin() (e.g. "OV5640").
const char *cameraSetupSensorName();

// Product ID read from the sensor after init (0 if init failed).
uint16_t cameraSetupSensorPid();

// Full-res and stream framesizes chosen for the detected sensor.
framesize_t cameraSetupMaxFramesize();
framesize_t cameraSetupStreamFramesize();

// JPEG quality targets (0-63, higher = more compression).
int cameraSetupStreamQuality();
int cameraSetupCaptureQuality();

// Minimum milliseconds between streamed frames for the active sensor.
// The HTTP stream loop uses this to avoid backlog on slow sensors (OV5640).
uint32_t cameraSetupStreamMinIntervalMs();

#endif  // CAMERA_SETUP_H
