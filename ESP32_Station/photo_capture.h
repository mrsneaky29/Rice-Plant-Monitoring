#ifndef PHOTO_CAPTURE_H
#define PHOTO_CAPTURE_H

#include <stdint.h>
#include <stddef.h>
#include "esp_camera.h"

void photoCaptureBegin();
bool photoCaptureIsStreaming();
bool photoCaptureTryBeginStream();
void photoCaptureEndStream();

// Reflects whether esp_camera_init() succeeded at boot. When false, every
// capture/stream entry point below fails fast instead of touching an
// uninitialized camera driver -- set once from setup().
void photoCaptureSetCameraOk(bool ok);
bool photoCaptureCameraOk();

// Works any time: full-resolution capture when idle, or (while live view
// is streaming) via a brief pause-resume of the stream -- see
// photoCaptureServiceStreamSnap. Always full resolution either way.
bool photoCaptureSnap(char *outFilename, size_t outFilenameSize);

// Called by the stream loop once per frame, before it grabs its own frame.
// Services a pending snap request (if any) at full resolution -- see the
// .cpp for details. Briefly pauses the live stream while it runs.
void photoCaptureServiceStreamSnap();

// Whether the LED flash fires during a full-resolution (non-streaming)
// capture. Has no effect on captures taken while live view is streaming --
// those are grabbed from the stream and never fire the flash.
void photoCaptureSetFlashEnabled(bool enabled);
bool photoCaptureFlashEnabled();

// The framesize full-resolution captures target. Call once from setup()
// after identifying the sensor -- swapping the OV2640 for an OV5640 (or
// any other sensor the driver supports) on the same wiring should capture
// at that sensor's real maximum instead of being capped at OV2640's size.
void photoCaptureSetMaxFramesize(framesize_t fs);

// The framesize live view streams at. Kept separate from the capture
// target and sensor-aware: a high-megapixel sensor (e.g. OV5640) needs a
// noticeably smaller stream size than OV2640's SVGA to stay smooth over
// WiFi -- both because it has more native pixels to read out per frame at
// any given size and because MJPEG stream bandwidth scales directly with
// frame size regardless of sensor.
void photoCaptureSetStreamFramesize(framesize_t fs);

// Sync internal resolution tracking after camera_setup leaves the sensor
// in low-res idle/stream mode at boot.
void photoCaptureSyncIdleMode();

// JPEG quality (0-63, lower = less compression = bigger/slower/sharper).
// Kept separate for streaming vs. capture since streaming wants small,
// fast frames while a still capture can afford to spend more time for
// detail -- though a very large sensor may still want a bit more
// compression on capture too, purely to keep encode/SD-write time down.
void photoCaptureSetQuality(int streamQuality, int captureQuality);

// Steady live-view light (torch), independent of the capture flash above.
// Stays on until explicitly turned off; not persisted across reboot.
void photoCaptureSetTorch(bool on);
bool photoCaptureTorchOn();

bool photoCaptureLockLatest(const uint8_t **outBuf, size_t *outLen);
void photoCaptureUnlockLatest();

// Captures one JPEG frame at `size` into a PSRAM-allocated buffer instead
// of saving it to the gallery -- for callers (like the water-level vision
// check) that need image bytes to hand elsewhere, not a gallery photo.
// Caller owns *outBuf and must free() it. Fails (returns false) while
// streaming rather than trying to interrupt it, unlike photoCaptureSnap --
// this path is for periodic background checks, not a user-facing button,
// so it's fine for it to just skip a cycle if live view happens to be on.
bool photoCaptureSnapToBuffer(framesize_t size, uint8_t **outBuf, size_t *outLen);

// Minimum gap between streamed frames (0 = no pacing). Set from camera_setup
// for slow sensors so the HTTP stream does not build a multi-second backlog.
void photoCaptureSetStreamMinIntervalMs(uint32_t ms);
uint32_t photoCaptureStreamMinIntervalMs();

#endif  // PHOTO_CAPTURE_H
