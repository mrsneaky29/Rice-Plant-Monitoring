"""Synthetic tests for SD storage directory limits and concurrency considerations."""
import pytest


def simulate_erase_all_task(date_folders: list[str]) -> tuple[list[str], list[str], int]:
    """Exact logic reproduction of eraseAllTask in firmware/ESP32_Station/sd_storage.cpp:614-675.

    Firmware declares:
        char dirs[64][16];
        int dirCount = 0;
        ...
        while (entry && dirCount < 64) {
            dirs[dirCount] = entry.name();
            dirCount++;
        }
    And then loops through dirs[0..dirCount-1] to erase photos and rmdir.
    Finally, photoCount is set to 0.
    """
    MAX_DIRS = 64
    discovered_dirs = []
    for d in date_folders:
        if len(discovered_dirs) < MAX_DIRS:
            discovered_dirs.append(d)
        else:
            break

    erased_dirs = []
    for d in discovered_dirs:
        erased_dirs.append(d)

    unerased_dirs = [d for d in date_folders if d not in erased_dirs]
    # sd_storage.cpp:672 sets photoCount = 0 unconditionally!
    reported_photo_count = 0

    return erased_dirs, unerased_dirs, reported_photo_count


def test_defect_erase_all_task_omits_folders_past_64():
    """Reproduces Defect: Hardcoded dirs[64][16] in sd_storage.cpp leaves folders > 64 undeleted.

    Scenario:
    A rice growing season runs 90 to 120 days.
    If the camera runs continuously in the field for 90 days, 90 date folders exist.
    Operator initiates 'Erase All' (sdStorageEraseAllStart).
    Only the first 64 folders are deleted.
    Folders 65 through 90 (26 days of photos!) remain on the SD card.
    Furthermore, photoCount is set to 0, creating a mismatch between reported photo count
    and actual SD card storage contents.
    """
    total_days = 90
    simulated_folders = [f"2026-06-{i+1:02d}" for i in range(30)] + \
                        [f"2026-07-{i+1:02d}" for i in range(31)] + \
                        [f"2026-08-{i+1:02d}" for i in range(29)]
    assert len(simulated_folders) == total_days

    erased, unerased, reported_photo_count = simulate_erase_all_task(simulated_folders)

    assert len(erased) == 64
    assert len(unerased) == 26
    # 26 folders were silently skipped!
    assert "2026-08-04" in unerased
    assert "2026-08-29" in unerased
    # photoCount was falsely zeroed out
    assert reported_photo_count == 0


def test_upload_timeout_budget_vs_transfer_duration():
    """Demonstrates socket timeout failure on large high-resolution images.

    Firmware configuration:
      - CYCLE_BUDGET_MS = 120000 (120 seconds cycle budget)
      - http.setTimeout(12000) (12 seconds socket timeout in cloud_device.cpp:89, 103)
      - Typical ESP32 2.4GHz TLS upload speed over weak Wi-Fi: ~40 to 60 KB/s
    """
    socket_timeout_seconds = 12.0
    image_size_bytes = 1_200_000  # 1.2 MB UXGA JPEG
    speed_bytes_per_sec = 50_000  # 50 KB/s realistic TLS throughput on ESP32

    transfer_time_seconds = image_size_bytes / speed_bytes_per_sec
    assert transfer_time_seconds == 24.0

    # Socket timeout of 12s aborts the 24s transfer, even though the cycle budget is 120s!
    assert transfer_time_seconds > socket_timeout_seconds
    assert transfer_time_seconds < 120.0  # Well within the 120s cycle budget
