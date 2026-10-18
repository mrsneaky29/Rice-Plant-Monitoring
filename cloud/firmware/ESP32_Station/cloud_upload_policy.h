#pragma once
#include <stdint.h>

constexpr bool cloudPhotoShouldUpload(bool acknowledged, bool queued, bool includeExisting) {
  return !acknowledged && (queued || includeExisting);
}

// Inactive deadlines must not be compared with millis(): zero otherwise blocks
// uploads after 2^31 ms. Clear an expired deadline before the next rollover.
constexpr bool cloudUploadBackoffIsReady(uint32_t now, bool active, uint32_t deadline) {
  return !active || (int32_t)(now - deadline) >= 0;
}

inline bool cloudUploadBackoffReady(uint32_t now, bool &active, uint32_t deadline) {
  bool ready = cloudUploadBackoffIsReady(now, active, deadline);
  if (ready) active = false;
  return ready;
}
