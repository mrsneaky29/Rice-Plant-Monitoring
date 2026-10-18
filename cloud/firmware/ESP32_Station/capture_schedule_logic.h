#ifndef CAPTURE_SCHEDULE_LOGIC_H
#define CAPTURE_SCHEDULE_LOGIC_H

#include <stdint.h>

struct CaptureScheduleTransition {
  uint32_t deadline;
  bool due;
  constexpr CaptureScheduleTransition(uint32_t next, bool fire)
      : deadline(next), due(fire) {}
};

// Portable deadline transition shared by firmware and compile-time schedule
// checks. `windowWaitSeconds` is time until the next opening, or zero when the
// current time is eligible. Only one due event is emitted; missed intervals
// never trigger a burst.
constexpr CaptureScheduleTransition captureScheduleTransition(
    uint32_t nowEpoch, uint32_t deadline, uint32_t intervalSeconds,
    bool inWindow, uint32_t windowWaitSeconds) {
  return intervalSeconds == 0
      ? CaptureScheduleTransition(deadline, false)
      : deadline == 0
          ? CaptureScheduleTransition(nowEpoch + intervalSeconds, false)
          : (int32_t)(nowEpoch - deadline) < 0
              ? CaptureScheduleTransition(deadline, false)
              : !inWindow
                  ? CaptureScheduleTransition(nowEpoch + windowWaitSeconds, false)
                  : CaptureScheduleTransition(nowEpoch + intervalSeconds, true);
}

#endif
