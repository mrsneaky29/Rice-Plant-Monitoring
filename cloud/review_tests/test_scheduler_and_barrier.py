"""Deterministic unit and regression tests for ESP32 firmware scheduler, backoff, and pre-sleep barrier."""
import pytest


def capture_schedule_transition(now_epoch, deadline, interval_seconds, in_window, window_wait_seconds):
    """Exact logic from firmware/ESP32_Station/capture_schedule_logic.h:17."""
    if interval_seconds == 0:
        return deadline, False
    if deadline == 0:
        return now_epoch + interval_seconds, False
    if (now_epoch - deadline) < 0:
        return deadline, False
    if not in_window:
        return now_epoch + window_wait_seconds, False
    return now_epoch + interval_seconds, True


def capture_timer_seconds_until_next(now_epoch, next_capture_epoch, timer_enabled, interval_seconds, in_window, window_wait_seconds):
    """Exact logic from firmware/ESP32_Station/capture_timer.cpp:152."""
    if not timer_enabled:
        return 0
    if not now_epoch:
        return interval_seconds
    if not in_window:
        return window_wait_seconds if window_wait_seconds > 0 else 1
    if not next_capture_epoch:
        return interval_seconds
    remaining = next_capture_epoch - now_epoch
    return remaining if remaining > 0 else 1


def calculate_continuous_loop_interval(base_interval_ms, sync_failures, upload_failures):
    """Exact backoff logic from firmware/ESP32_Station/cloud_device.cpp:573."""
    failures = max(sync_failures, upload_failures)
    if failures == 0:
        return base_interval_ms
    shift = min(failures, 4)
    interval = base_interval_ms * (1 << shift)
    return min(interval, 300000)


def calculate_next_upload_allowed_delay(consecutive_upload_failures):
    """Exact backoff logic for upload retry from firmware/ESP32_Station/cloud_device.cpp:498."""
    shift = min(consecutive_upload_failures, 4)
    return 15000 * (1 << shift)


class TestSchedulerWindowCalculations:
    """Verifies that capture window calculation does not overshoot window opening or lock out narrow windows."""

    def test_window_wait_does_not_add_interval_narrow_window(self):
        """Window duration: 4 hours (06:00 to 10:00).
        Interval: 6 hours (21600s).
        Current time: 20:00 (outside window).
        Time to window opening (06:00): 10 hours = 36000s.
        """
        now = 1792200000  # 20:00
        window_wait = 36000  # 10 hours until 06:00
        interval = 21600  # 6 hours
        past_deadline = now

        next_deadline, due = capture_schedule_transition(
            now_epoch=now,
            deadline=past_deadline,
            interval_seconds=interval,
            in_window=False,
            window_wait_seconds=window_wait,
        )

        assert due is False
        # Next deadline must land EXACTLY at the window opening (06:00), NOT 06:00 + interval (12:00)
        assert next_deadline == now + window_wait
        assert next_deadline - now == 36000

        # Now simulate arrival at 06:00 when window opens:
        now_at_open = next_deadline
        fire_deadline, fire_due = capture_schedule_transition(
            now_epoch=now_at_open,
            deadline=next_deadline,
            interval_seconds=interval,
            in_window=True,
            window_wait_seconds=0,
        )
        assert fire_due is True
        assert fire_deadline == now_at_open + interval

    def test_seconds_until_next_returns_exact_window_opening(self):
        """captureTimerSecondsUntilNext returns secondsUntilWindowStart() without interval offset."""
        now = 1792200000
        window_wait = 1800  # 30 minutes until window
        interval = 7200  # 2 hours
        deadline = now + 100

        seconds = capture_timer_seconds_until_next(
            now_epoch=now,
            next_capture_epoch=deadline,
            timer_enabled=True,
            interval_seconds=interval,
            in_window=False,
            window_wait_seconds=window_wait,
        )
        assert seconds == 1800  # Wakes device right when window opens!

    def test_midnight_wrapped_window_logic(self):
        """Window from 20:00 to 04:00 (wraps past midnight).
        At 16:00, wait is 4 hours (14400s).
        """
        now = 1792200000  # 16:00
        window_wait = 14400
        interval = 3600
        deadline = now - 100

        next_deadline, due = capture_schedule_transition(
            now_epoch=now,
            deadline=deadline,
            interval_seconds=interval,
            in_window=False,
            window_wait_seconds=window_wait,
        )
        assert due is False
        assert next_deadline == now + 14400  # 20:00

        # At 21:00 (inside wrapped window):
        now_in = now + 18000
        in_deadline, in_due = capture_schedule_transition(
            now_epoch=now_in,
            deadline=next_deadline,
            interval_seconds=interval,
            in_window=True,
            window_wait_seconds=0,
        )
        assert in_due is True
        assert in_deadline == now_in + interval


class TestBackoffSemantics:
    """Verifies exponential backoff for continuous mode sync and photo upload failures."""

    def test_continuous_mode_sync_backoff(self):
        base = 30000  # 30s base interval
        assert calculate_continuous_loop_interval(base, 0, 0) == 30000
        assert calculate_continuous_loop_interval(base, 1, 0) == 60000
        assert calculate_continuous_loop_interval(base, 2, 0) == 120000
        assert calculate_continuous_loop_interval(base, 3, 0) == 240000
        assert calculate_continuous_loop_interval(base, 4, 0) == 300000  # capped at 300s
        assert calculate_continuous_loop_interval(base, 10, 0) == 300000

    def test_continuous_mode_upload_failures_trigger_backoff(self):
        """Upload failures back off loop syncs even if HTTP sync succeeded."""
        base = 30000
        assert calculate_continuous_loop_interval(base, 0, 1) == 60000
        assert calculate_continuous_loop_interval(base, 0, 2) == 120000
        assert calculate_continuous_loop_interval(base, 0, 3) == 240000
        assert calculate_continuous_loop_interval(base, 0, 5) == 300000

    def test_upload_retry_backoff_delays(self):
        """Photo upload failure exponential delay: 15s -> 30s -> 60s -> 120s -> 240s."""
        assert calculate_next_upload_allowed_delay(1) == 30000
        assert calculate_next_upload_allowed_delay(2) == 60000
        assert calculate_next_upload_allowed_delay(3) == 120000
        assert calculate_next_upload_allowed_delay(4) == 240000
        assert calculate_next_upload_allowed_delay(8) == 240000  # shift capped at 4


class TestPreSleepBarrierAndPostUploadCapture:
    """Verifies the state machine that prevents capture starvation during long uploads and before deep sleep."""

    class MockEsp32Lifecycle:
        def __init__(self, interval_seconds=60):
            self.interval_seconds = interval_seconds
            self.now = 1000
            self.deadline = self.now + interval_seconds
            self.timer_enabled = True
            self.sleep_enabled = True
            self.cycle_active = False
            self.scheduled_cycle_pending = False
            self.synced = True
            self.photos_on_sd = []
            self.photos_uploaded = []
            self.sleep_entered = False
            self.sleep_duration = None

        def capture_timer_consume_due(self):
            if not self.timer_enabled:
                return False
            if self.now >= self.deadline:
                self.deadline = self.now + self.interval_seconds
                return True
            return False

        def capture_timer_task_tick(self):
            # Line 115 guard in capture_timer.cpp
            if self.cycle_active or self.scheduled_cycle_pending:
                return "skipped_cycle_active"
            if self.capture_timer_consume_due():
                if self.sleep_enabled:
                    self.scheduled_cycle_pending = True
                    return "scheduled_wake_posted"
                else:
                    self.photos_on_sd.append(f"timer_snap_{self.now}.jpg")
                    return "local_snap"
            return "idle"

        def run_cycle(self, scheduled_due=False, upload_duration_seconds=0):
            self.cycle_active = True
            if self.scheduled_cycle_pending:
                self.scheduled_cycle_pending = False
                scheduled_due = True

            # If cycle woke due to schedule, take photo
            if scheduled_due:
                fresh_photo = f"photo_{self.now}.jpg"
                self.photos_on_sd.append(fresh_photo)
                if self.synced:
                    self.photos_uploaded.append(fresh_photo)

            # Upload duration spans forward in time
            self.now += upload_duration_seconds

            # Post-upload due check (lines 515-525 in cloud_device.cpp)
            if self.timer_enabled and self.capture_timer_consume_due():
                overdue_photo = f"overdue_{self.now}.jpg"
                self.photos_on_sd.append(overdue_photo)
                # Finding 2: overdue photo is immediately fed into upload
                if self.synced:
                    self.photos_uploaded.append(overdue_photo)

            # Attempt sleep
            self.sleep_until_next_cycle()
            self.cycle_active = False

        def sleep_until_next_cycle(self):
            # Pre-sleep barrier (Finding 1)
            if self.scheduled_cycle_pending or self.capture_timer_consume_due():
                self.scheduled_cycle_pending = True
                # Abort sleep!
                return

            remaining = max(self.deadline - self.now, 1)
            self.sleep_entered = True
            self.sleep_duration = remaining

    def test_capture_timer_task_does_not_race_while_cycle_is_active(self):
        device = self.MockEsp32Lifecycle(interval_seconds=30)
        device.cycle_active = True
        device.now += 40  # Deadline passed during upload
        status = device.capture_timer_task_tick()
        assert status == "skipped_cycle_active"
        # Deadline was NOT prematurely consumed by background task
        assert device.photos_on_sd == []

    def test_post_upload_overdue_capture_is_taken_and_uploaded(self):
        device = self.MockEsp32Lifecycle(interval_seconds=30)
        # Cycle takes 35 seconds to upload, which exceeds the 30s interval
        device.run_cycle(scheduled_due=True, upload_duration_seconds=35)

        # 1. First photo captured and uploaded
        assert "photo_1000.jpg" in device.photos_on_sd
        assert "photo_1000.jpg" in device.photos_uploaded

        # 2. Overdue photo captured at t=1035 AND immediately uploaded in same cycle!
        assert "overdue_1035.jpg" in device.photos_on_sd
        assert "overdue_1035.jpg" in device.photos_uploaded

        # 3. Next deadline is at 1035 + 30 = 1065, so sleep duration is 30s
        assert device.sleep_entered is True
        assert device.sleep_duration == 30

    def test_pre_sleep_barrier_aborts_sleep_if_pending_or_due(self):
        device = self.MockEsp32Lifecycle(interval_seconds=30)
        device.scheduled_cycle_pending = True
        device.sleep_until_next_cycle()
        assert device.sleep_entered is False
        assert device.scheduled_cycle_pending is True
