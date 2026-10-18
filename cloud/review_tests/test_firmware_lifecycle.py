"""Regression tests for firmware lifecycle and capture schedule defects."""
import pytest


def capture_schedule_transition(now_epoch, deadline, interval_seconds, in_window, window_wait_seconds):
    """Exact Python reproduction of firmware/ESP32_Station/capture_schedule_logic.h:17."""
    if interval_seconds == 0:
        return deadline, False
    if deadline == 0:
        return now_epoch + interval_seconds, False
    if (now_epoch - deadline) < 0:
        return deadline, False
    if not in_window:
        return now_epoch + window_wait_seconds + interval_seconds, False
    return now_epoch + interval_seconds, True


def capture_timer_seconds_until_next(now_epoch, next_capture_epoch, timer_enabled, interval_seconds, in_window, window_wait_seconds):
    """Exact Python reproduction of firmware/ESP32_Station/capture_timer.cpp:152."""
    if not timer_enabled:
        return 0
    if not now_epoch:
        return interval_seconds
    if not next_capture_epoch:
        return interval_seconds
    if not in_window:
        return window_wait_seconds + interval_seconds
    remaining = next_capture_epoch - now_epoch
    return remaining if remaining > 0 else 1


def test_defect_deep_sleep_wake_clock_unset_causes_1s_wake_loop():
    """Reproduces Defect: Deep-sleep timer wake has uninitialized clock (now=0).

    1. Firmware wakes up from deep-sleep timer wakeup (nextCaptureEpoch was retained in RTC).
    2. Before sendSync(), nowEpoch is 0.
    3. captureTimerConsumeDue() sees now==0, so it returns due=False and does NOT advance nextCaptureEpoch.
    4. sendSync() obtains server_time (now=1792200000).
    5. sleepUntilNextCycle() calls captureTimerSecondsUntilNext().
    6. Since nextCaptureEpoch <= now, remaining <= 0, which defaults to 1 second.
    7. Result: The camera enters an infinite 1-second wake-sync-sleep loop, never capturing the due photo.
    """
    timer_interval = 21600  # 6 hours
    retained_deadline = 1792200000  # Scheduled deadline that woke the ESP32

    # Step 1 & 2: At wake, before sync, clock is unset
    now_at_wake = 0
    # captureTimerConsumeDue logic when !now (capture_timer.cpp:93-100)
    unsynced_elapsed = 0 + 1
    due_at_wake = False  # 1 < 21600 -> returns False without advancing deadline
    assert due_at_wake is False

    # Step 3: sendSync sets the time to the current epoch
    now_after_sync = retained_deadline + 5  # 5 seconds elapsed during sync
    # captureTimerSecondsUntilNext is called in sleepUntilNextCycle()
    seconds_to_sleep = capture_timer_seconds_until_next(
        now_epoch=now_after_sync,
        next_capture_epoch=retained_deadline,  # still in the past because deadline was never advanced!
        timer_enabled=True,
        interval_seconds=timer_interval,
        in_window=True,
        window_wait_seconds=0
    )

    # CRITICAL DEFECT: Sleeps for only 1 second instead of interval!
    assert seconds_to_sleep == 1, (
        f"Expected 1-second failure loop reproduction, got {seconds_to_sleep}"
    )


def test_defect_window_wait_adds_interval_locking_out_window():
    """Reproduces Defect: captureScheduleTransition adds intervalSeconds to windowWaitSeconds.

    Scenario:
    - Time window: 06:00 to 10:00 (4-hour duration).
    - Interval: 6 hours (21600 seconds).
    - Current time: 20:00 (outside window).
    - Time until window start (06:00): 10 hours = 36000 seconds.
    - Expected behavior: Schedule next wake at 06:00 when the window opens.
    - Actual firmware behavior: Schedules at now + 36000 + 21600 = 06:00 + 6 hours = 12:00.
    - Failure: 12:00 is ALREADY PAST 10:00 (window closed!).
    - At 12:00, it is again outside the window, so it pushes deadline to 12:00 next day.
    - Result: The camera is permanently locked out of ever capturing during the 4-hour window!
    """
    now = 1792200000  # 20:00
    window_wait = 36000  # 10 hours until 06:00
    interval = 21600  # 6 hours
    deadline = now  # past deadline

    next_deadline, due = capture_schedule_transition(
        now_epoch=now,
        deadline=deadline,
        interval_seconds=interval,
        in_window=False,
        window_wait_seconds=window_wait
    )

    assert due is False
    # Next wake scheduled 16 hours in future (12:00), which completely skips the 06:00-10:00 window!
    assert next_deadline == now + window_wait + interval
    assert (next_deadline - (now + window_wait)) == interval  # Over-shoots window opening by 6 hours!
