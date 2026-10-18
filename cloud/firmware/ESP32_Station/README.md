# Rice Cloud ESP32 station firmware

This separate Arduino sketch adds direct HTTPS reporting, cloud commands and new-photo uploads. The original local-hub firmware is retained separately. A running PC bridge is not required for the cloud path.

## Verified rollout — 18 October 2026

Plant 2 received protected normal firmware via OTA. Its OV3660 camera, SD mount, authenticated cloud reports, three dashboard captures and direct JPEG uploads were verified. The compiled image was 1,487,632 bytes, below the actual 1,966,080-byte OTA slot. The first new cloud JPEG's checksum was checked against the server file.

Plants 1 and 3 have not completed the cloud rollout. Sleep remains off on Plant 2; physical sleep/wake acceptance and power-cut recovery are pending. Local status can time out during capture/upload activity even while cloud reports continue. Photo counts do not establish every original file's integrity.

## Identity and sensor profiles

Station numbers, names, roles and hostnames are stored in NVS. DHCP addresses can change. Verify `station_id` and hardware MAC before flashing; a station-specific token does not set the board's saved identity.

The one-time `cameraOnlyV1` migration selects the `none` sensor role without clearing Wi-Fi, identity, schedules or SD photographs. Later explicit selections support `none`, `soil` and `dht22`. Physical wiring does not prove an enabled profile; check `sensor_role` in `/status`. The verified cloud workflow covers photographs, not cloud sensor reporting.

| Original assignment | Wiring identification |
| --- | --- |
| Plant 1 | ADS1115: SDA on GPIO3/RX0, SCL on GPIO1/TX0 |
| Plant 2 | DHT22 data on GPIO13 |
| Plant 3 | No external sensor assigned |

Disconnect RX0/TX0 peripheral connections before using a serial programmer. GPIO16/17 are used by PSRAM. Hostnames follow the saved number: `ricecam-1.local`, `ricecam-2.local`, `ricecam-3.local`.

## Build and OTA

- Board: AI Thinker ESP32-CAM with PSRAM; DIO flash mode.
- Tested core: Arduino ESP32 3.3.11; library: ArduinoJson 7.4.3.
- Keep `partitions.csv` beside the sketch. Each OTA slot is `0x1e0000` bytes; the IDE's generic maximum is not the OTA limit.
- Copy `secrets.example.h` to ignored `secrets.h` and configure Wi-Fi and OTA credentials.
- Provision per-station credentials using the [cloud README](../../README.md). Ignored `.private/<station>/cloud_secrets.h` files contain the device token and CA.
- From the cloud directory, use `deploy/build_firmware.py` and `deploy/flash_ota.py`. The builder stages the token privately. OTA checks identity, image hash, size and photo-protection policy; add `--expected-mac` for the verified MAC.

Do not compile all boards with one device token. Initial serial flashing is needed if working OTA firmware is absent. OTA needs a powered, awake board on the local network and reboots it. Preserve the previous private image for rollback. Plants 1 and 3 were not flashed in this session.

TLS verifies the server's trusted root CA. The PSRAM-first allocator reduces internal-memory pressure. There is no insecure-TLS fallback; physical reliability still requires testing.

## SD preservation and upload recovery

Normal builds leave existing originals in place and upload new captures only. There is no automatic legacy-photo relocation or deletion, and mount failure never triggers formatting. Photo delete/erase entry points are blocked by the protected build.

A new JPEG is written to a separate `.part` file, closed, given an `.upload` intent, then renamed into its final name. Failed commits retain new recovery files. A successful cloud acknowledgement adds a `.cloud` sidecar and retains the JPEG and its intent. The sidecars survive resets and allow new captures to retry without making old originals eligible.

Historical uploads require `--upload-existing-photos` at build and OTA time. Leave it off while collecting old photos using an SD reader. Preserve `.part` and journal files when diagnosing interrupted work; inspect them separately after preserving the card.

## Commands and sleep

The dashboard queues capture, settings, Stay awake and Resume sleep. Awake boards receive them at the next sync; sleeping boards cannot receive a network command until their next wake. RESET or power-on starts a ten-minute maintenance window. Stay awake lasts until Resume sleep or reboot.

Sleep is off by default and must be explicitly enabled. Resume sleep clears the temporary Stay awake override; it does not enable a disabled sleep setting. Disabling the photo timer also prevents automatic deep sleep. Upload work is bounded and the schedule is recomputed before sleeping, but a complete physical sleep/wake cycle remains unverified.

Command IDs and in-progress results are journaled under `/CLOUD` on SD. A reset during capture is reported as uncertain rather than blindly repeating it. Commands requiring the journal fail when SD is unavailable.

## Diagnostic mode and logs

`--status-only` builds a separate image that leaves SD unmounted and disables capture/upload cycles and cloud commands. Its zero count means storage was not inspected, not that the card is empty. Live preview remains available without an SD write.

Read `/status` for health, identity and settings, and `/log?lines=80` for the RAM log. USB serial can show ROM boot messages; this sketch does not mirror the application RAM log to serial. A charging-only USB connection does not provide serial access. Close live preview after testing to release the camera.

The device sends no OpenAI analysis request. The optional cloud worker is disabled in the deployed service; the original PC hub has a separate optional analysis integration.
