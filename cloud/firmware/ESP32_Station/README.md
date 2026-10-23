# Rice Cloud ESP32 station firmware

This separate Arduino sketch adds direct HTTPS reporting, cloud commands and new-photo uploads. The original local-hub firmware is retained separately. A running PC bridge is not required for the cloud path.

Documentation audit: 23 October 2026, against local source only. Hardware and deployment observations below are from 18–22 October; this audit performed no camera or production checks. The [cloud README](../../README.md) distinguishes the separately deployed 22 October website-feedback patch from the published source copy, which still awaits that implementation and its dedicated tests.

## Verified rollout — 18 October 2026

Plant 2 received protected normal firmware via OTA. Its OV3660 camera, SD mount, authenticated cloud reports, three dashboard captures and direct JPEG uploads were verified. The compiled image was 1,487,632 bytes, below the actual 1,966,080-byte OTA slot. The first new cloud JPEG's checksum was checked against the server file.

At this 18 October checkpoint, Plants 1 and 3 had not completed the cloud rollout and Plant 2 sleep was off. Plant 2 later reported sleep enabled on 22 October; physical sleep/wake acceptance and power-cut recovery remain pending. Plants 1 and 3 still await rollout. Local status can time out during capture/upload activity even while cloud reports continue. Photo counts do not establish every original file's integrity.

## Identity and sensor profiles

Station numbers, names and roles are stored in NVS; the hostname is derived from the saved number. An unconfigured board defaults to Plant 1. DHCP addresses can change. Verify `station_id` and hardware MAC before flashing; a station-specific token does not set the board's saved identity.

The one-time `cameraOnlyV1` migration selects the `none` sensor role without clearing Wi-Fi, identity, schedules or SD photographs. A separate one-time `timer1hV1` migration changes a saved legacy ten-second photo interval to one hour; it preserves other saved intervals. Later explicit sensor selections support `none`, `soil` and `dht22`. Physical wiring does not prove an enabled profile; check `sensor_role` in `/status`. The verified cloud workflow covers photographs, not cloud sensor reporting.

| Original assignment | Wiring identification |
| --- | --- |
| Plant 1 | ADS1115: SDA on GPIO3/RX0, SCL on GPIO1/TX0 |
| Plant 2 | DHT22 data on GPIO13 |
| Plant 3 | No external sensor assigned |

Disconnect RX0/TX0 peripheral connections before using a serial programmer. GPIO16/17 are used by PSRAM. Hostnames follow the saved number: `ricecam-1.local`, `ricecam-2.local`, `ricecam-3.local`.

## Build and OTA

- Board: AI Thinker ESP32-CAM with PSRAM; DIO flash mode.
- Tested core: Arduino ESP32 3.3.11; library: ArduinoJson 7.4.3. The private builder uses the installed core/library versions; it does not install or pin them automatically.
- Keep `partitions.csv` beside the sketch. Each OTA slot is `0x1e0000` bytes; the IDE's generic maximum is not the OTA limit.
- Copy `secrets.example.h` to ignored `secrets.h` and configure Wi-Fi and OTA credentials.
- Provision per-station credentials using the [cloud README](../../README.md). Ignored `.private/<station>/cloud_secrets.h` files contain the device token and CA.
- From the cloud directory, use `deploy/build_firmware.py` and `deploy/flash_ota.py`. The builder stages the token privately. OTA checks identity, image hash, size and photo-protection policy; add `--expected-mac` for the verified MAC.

Do not compile all boards with one device token. Initial serial flashing is needed if working OTA firmware is absent. OTA needs a powered, awake board on the local network and reboots it. Preserve the previous private image for rollback. Plants 1 and 3 had not completed rollout at the 22 October checkpoint.

The station-to-cloud reporting/upload client verifies the server's trusted root CA. The cloud endpoint is compiled as `CLOUD_BASE` in `cloud_device.cpp`; changing deployment also requires matching server origin/Caddy configuration and a CA trusted for its certificate chain. The PSRAM-first allocator reduces internal-memory pressure. This cloud client has no insecure-TLS fallback; the retained local analysis client described below is separate. Physical reliability still requires testing.

## SD preservation and upload recovery

Normal builds leave existing originals in place and upload new captures only. There is no automatic legacy-photo relocation or deletion, and mount failure never triggers formatting. Photo delete/erase entry points are blocked by the protected build.

A new JPEG is written to a separate `.part` file, closed, given an `.upload` intent, then renamed into its final name. A complete write that cannot create its intent or publish its JPEG retains recovery files; a detected short write removes only that new partial file. A successful cloud acknowledgement adds a `.cloud` sidecar and retains the JPEG and its intent. The sidecars survive resets and allow new captures to retry without making old originals eligible.

Historical firmware uploads require `--upload-existing-photos` at build and OTA time. Leave it off while collecting old photos using an SD reader. This option sends ordinary uploads, so old photos can incur paid analysis if server analysis is enabled; it does not mark them as archive imports. Preserve `.part` and journal files when diagnosing interrupted work; inspect them separately after preserving the card. A retained `.part` is for manual recovery, not an automatically committed or uploaded JPEG. Power-loss integrity and file-by-file preservation still require physical checks.

## Commands and sleep

The dashboard queues capture, settings, Stay awake and Resume sleep. Awake boards receive them at the next sync; sleeping boards cannot receive a network command until their next wake. RESET or power-on starts a ten-minute maintenance window. Stay awake lasts until Resume sleep or reboot. Capture acknowledgement records the SD capture outcome separately from the subsequent cloud upload; it does not prove that a cloud photo was received.

On an unconfigured board, sleep and the photo timer are off, the saved photo interval is one hour, and the daily window is disabled. Existing NVS settings survive an OTA update apart from the migrations above. Resume sleep clears the temporary Stay awake override and ends the maintenance window; it does not enable a disabled sleep setting. Disabling the photo timer also prevents automatic deep sleep. The cloud dashboard accepts one-hour to seven-day intervals; local firmware timer setters accept shorter intervals, so local changes require care. Windows use the station's local clock, defaulting to IST (UTC+05:30), and can cross midnight; equal start/end means an all-day window.

The cloud cycle has a two-minute work budget and retry backoff; it recomputes the schedule before sleeping and defers sleep during OTA, an active preview or incomplete storage barriers. Network timeouts and shutdown work mean the budget is not a measured upper bound on physical awake time. A complete physical sleep/wake cycle and current measurement remain unverified.

On 22 October, Plant 2 reported sleep enabled with a one-hour interval. That reported setting does not establish reliable capture: daytime photo gaps and two user-issued captures ending in `uncertain` / `reset_during_command` remain unresolved. Reset diagnosis is deferred and the scheduled follow-up monitor was deleted at the user's request. No firmware update was made during that follow-up.

Command IDs and in-progress results are journaled under `/CLOUD` on SD. A reset after a command was marked started is reported as uncertain rather than blindly repeating it; completed photo identity can be reported again and its upload retried. All new cloud commands require the SD journal, including sleep/settings commands; they fail when the journal cannot be written. This prevents issuing unjournaled side effects and means SD failure can also prevent remote maintenance commands.

## Diagnostic mode and logs

`--status-only` builds a separate image that leaves SD unmounted and disables capture/upload cycles and cloud commands. Its zero count means storage was not inspected, not that the card is empty. Live preview remains available without an SD write.

Read `/status` for health, identity and settings, and `/log?lines=80` for the RAM log. USB serial can show ROM boot messages; this sketch does not mirror the application RAM log to serial. A charging-only USB connection does not provide serial access. Close live preview after testing to release the camera.

The intended cloud workflow performs analysis on the server. At the 22 October 2026 checkpoint, the cloud worker was enabled for newly uploaded ordinary photos using the configured server-side key; the legacy import command and uploads explicitly marked as archive imports were excluded. On 23 October, read-only cloud records and the live browser verified a matching new Plant 2 photo and one completed server-side report from 22 October; no new analysis request was submitted for that check. Reliable hourly capture and the physical sleep cycle remain unverified. This required no firmware update. The original PC hub has a separate optional analysis integration.

This firmware also retains the older local `/water/vision` and `/water/analyze` routes. Startup disables automatic on-device analysis, but a configured device-side key allows it to be enabled again locally or an existing photo to be queued manually. That separate retained client requests `gpt-4o-mini` and uses `setInsecure()`, so it does not verify the provider's TLS certificate. It is not part of the verified cloud workflow; leave it unused and keep analysis credentials on the server for that workflow. Boot-time disablement does not remove these local capabilities.

The device's local HTTP status, preview and control interfaces are for a trusted local network only; do not expose those local interfaces directly to the internet. Remote access uses the separate password-protected dashboard over public HTTPS, with administrator sessions and CSRF checks; station reports and uploads use individual device credentials.
