#include "../ESP32_Station/capture_schedule_logic.h"

constexpr CaptureScheduleTransition initialized = captureScheduleTransition(100, 0, 60, true, 0);
static_assert(initialized.deadline == 160 && !initialized.due, "first boot starts interval");

constexpr CaptureScheduleTransition beforeDeadline = captureScheduleTransition(159, 160, 60, true, 0);
static_assert(beforeDeadline.deadline == 160 && !beforeDeadline.due, "deadline is preserved before due");

constexpr CaptureScheduleTransition exactDeadline = captureScheduleTransition(160, 160, 60, true, 0);
static_assert(exactDeadline.deadline == 220 && exactDeadline.due, "exact wake captures and advances");

// A late timer wake consumes one capture, then schedules from actual wake time;
// it cannot produce a catch-up burst for missed intervals.
constexpr CaptureScheduleTransition lateWake = captureScheduleTransition(400, 220, 60, true, 0);
static_assert(lateWake.deadline == 460 && lateWake.due, "late wake emits one capture");

constexpr CaptureScheduleTransition missedWindow = captureScheduleTransition(460, 460, 60, false, 900);
static_assert(missedWindow.deadline == 1360 && !missedWindow.due, "outside-window deadline targets opening");
constexpr CaptureScheduleTransition justBeforeWindowCapture = captureScheduleTransition(1359, 1360, 60, true, 0);
static_assert(justBeforeWindowCapture.deadline == 1360 && !justBeforeWindowCapture.due, "window deadline is not early");
constexpr CaptureScheduleTransition windowCapture = captureScheduleTransition(1360, 1360, 60, true, 0);
static_assert(windowCapture.deadline == 1420 && windowCapture.due, "window opening capture advances schedule");
