# Rice Plant Monitoring

**ESP32 cameras with SD originals and a password-protected HTTPS cloud dashboard.**

Remote access: **[admin.riceplant.achantalabs.com](https://admin.riceplant.achantalabs.com)**. Devices send photographs directly to this cloud service; a PC bridge is not required. The original local PC hub and local camera pages remain a separate, optional path.

[![Source checks](https://github.com/mrsneaky29/Rice-Plant-Monitoring/actions/workflows/checks.yml/badge.svg)](https://github.com/mrsneaky29/Rice-Plant-Monitoring/actions/workflows/checks.yml)
[![Cloud checks](https://github.com/mrsneaky29/Rice-Plant-Monitoring/actions/workflows/cloud.yml/badge.svg)](https://github.com/mrsneaky29/Rice-Plant-Monitoring/actions/workflows/cloud.yml)
![Hardware](https://img.shields.io/badge/hardware-ESP32--CAM-246b46)
![Hub](https://img.shields.io/badge/hub-Python%20%2B%20SQLite-346b9b)
![Deployment](https://img.shields.io/badge/deployment-local%20%2B%20cloud-646a6b)

Rice growth is gradual; useful observations need to survive gaps in computer availability. Each ESP32-CAM saves photographs on its microSD card. The current [`cloud/`](cloud/README.md) variant uploads new captures over certificate-verified HTTPS to a DigitalOcean service and retains SD originals. The original PC hub provides local synchronization, browsing, and optional analysis, with different SD-safety behavior described below.

The project grew through physical assembly, firmware flashing, live camera tests, network troubleshooting, and changes to how data is retained. It is a prototype with documented limitations and automated source-build checks; those checks do not establish continuous device reliability.

![System architecture](docs/architecture.svg)

The diagram and table include read-only cloud/browser verification on 23 October 2026 and the earlier dated hardware observations. This follow-up sent no camera commands or new paid analysis requests.

The main path is ESP32 → verified HTTPS → DigitalOcean service → private browser dashboard. Commands travel back at the next device sync or wake. The original local PC hub remains a separate optional path, rather than a required bridge.

## Last recorded cloud verification — 23 October 2026

| Area | Verified state |
| --- | --- |
| Dashboard | HTTPS deployment recorded; full photo cards; capture-time display, gallery order and date filters use camera filename timestamps. Timestamp and command-feedback fixes were narrowly deployed on 22 October; health returned HTTP 200; the retained command outcomes and cleared warning were verified in the live browser on 23 October |
| Plant 2 | Three direct cloud captures verified on 18 October; on 22 October, daytime gaps and two reset-interrupted capture commands were observed; reliable hourly capture is not established |
| Original photographs | Retained on SD; automatic legacy-file relocation disabled; historical uploads require explicit opt-in |
| Upload recovery | New captures use temporary files and durable upload markers; acknowledgement retains the original |
| Photo protection | Delete/erase controls blocked in the cloud firmware build; no destructive route was invoked during validation |
| Sleep | Plant 2 reported enabled with a one-hour interval on 22 October; reliable full wake/capture/upload/sleep acceptance remains pending |
| Plants 1 and 3 | Cloud firmware rollout deferred; no current cloud-operation claim for either board |
| Automatic analysis | Enabled on 22 October for ordinary uploads with verified `gpt-6.1-sol` account access and one count check; explicitly marked archive imports excluded; one new Plant 2 photo, its matching checksum and its completed report were verified on 23 October; the job ran on 22 October, completing about 41 seconds after upload |

Local HTTP status can time out while cloud reporting continues. Power-cut recovery, weak-Wi-Fi stress tests, SD file-by-file integrity checks and the camera-reset cause remain outstanding. The last checked backend used its original release directory with targeted analysis startup/archive safeguards, a worker update, and the later narrow website fixes; other server/backup changes in source are not established as deployed. No camera firmware or settings changed during the 22 October follow-up. The dated observations here are not a fresh availability check.

Cloud commands wait for the device's next sync or wake. A sleeping device cannot act immediately; physical RESET opens local maintenance access. Requested settings and a command acknowledgement do not prove a completed photograph or upload. See the [cloud operation and acceptance details](cloud/README.md).

## Original local-hub operation

1. Each station with an enabled timer captures at its configured interval and saves locally; the saved default interval is one hour.
2. The PC hub discovers cameras on the same Wi-Fi, including cameras whose IP addresses changed.
3. It downloads the photo history, alternating between stations, and records progress in SQLite.
4. The dashboard shows each plant's latest photograph, connection status, SD capacity, and available analysis history. **Take photo now** requests a fresh image from a specific camera.
5. When the PC stops, the local dashboard becomes unavailable. Powered ESPs with working cameras, SD storage and enabled timers can continue recording and synchronize when the hub returns.

The original hub is configured for **Camera-only mode**: it ignores external readings and actively changes a discovered `soil` or `dht22` station to `none`, then requests a restart. Firmware still supports explicit soil-moisture and DHT22 roles, but the stock hub will override them. In the original firmware boards stay powered between captures; the cloud firmware adds optional deep sleep. Both firmware variants include a one-time Camera-only role migration. Physical wiring does not prove an enabled sensor profile.

## Features

| Feature | Implementation |
| --- | --- |
| Independent recording | Camera and microSD storage on each ESP32-CAM |
| Persistent identities | `plant-1`, `plant-2`, and `plant-3`, with saved names and OTA hostnames |
| Automatic discovery | UDP announcements, startup broadcasts, and periodic rediscovery |
| Manual capture | Per-camera dashboard button that saves to SD and queues synchronization |
| Durable local storage | SQLite WAL transactions, staged `.part` downloads, and JPEG boundary checks |
| Recoverable local analysis | Stored background response IDs and JSON result sidecars; recovery starts after the response ID is persisted, with an earlier submission failure window |
| Local dashboard | Gallery, queue, plant cards, storage, settings, and available analysis graphs |
| Optional image analysis | Structured tiller-count checks, growth-stage estimates, water-level estimates, and confidence fields |
| Direct cloud operation | Per-device authenticated HTTPS reports/uploads and password-protected remote controls; Plant 2 captures/uploads verified on 18 October; current reliability remains unresolved |
| Protected cloud captures | New-photo upload intents, retained SD originals, blocked photo deletion and historical uploads off by default |
| Optional cloud sleep | Queued settings, timer wake and RESET maintenance; physical sleep/wake acceptance pending |

## Use the cloud dashboard

Open **[admin.riceplant.achantalabs.com](https://admin.riceplant.achantalabs.com)** and sign in with the privately provisioned admin password. The [`cloud/`](cloud/README.md) implementation provides direct ESP32 HTTPS photo uploads, queued remote controls, and optional
scheduled deep sleep with RESET-button maintenance access. It uses a dedicated
service and storage on the existing cloud server; no PC bridge is required.
Original hub/firmware files below remain available as a separate variant. See the cloud
README for private provisioning, station-specific OTA builds, tests, backup
restoration, and the physical acceptance checks required before enabling sleep.

## Run the original local PC hub

The hub runtime uses the **Python standard library**. Install Python 3.12 or newer and run this command from the repository root:

```powershell
py -3 PC_Hub/Monitoring_Hub.py --open-browser
```

Open **http://localhost:8765** on the host. Other devices on the same Wi-Fi can open `http://<PC-LAN-IP>:8765`. Allow the application's private-network firewall prompt so station discovery and local access can work.

On Windows, `PC_Hub/Open Monitoring Hub.bat` launches either the packaged executable or the Python source and opens the browser. The executable runs as a background web server; it does not create a separate desktop window. A second launch opens the existing dashboard instead of starting another hub.

The source-run hub stores downloaded photographs, queue state, SQLite backups, and an optional plaintext API-key file in **`PC_Hub/data/`**. A packaged executable uses a `data/` folder beside the executable. Preserve that folder when upgrading or moving an installation; keep it private. SQLite backups are made at startup, orderly shutdown and hourly, retaining seven files; they do not include the JPEGs or key file, so back up the whole data folder separately.

## Prepare the hardware

You need AI Thinker ESP32-CAM boards with PSRAM, compatible camera modules, microSD cards, stable power, and 2.4 GHz Wi-Fi. The original configuration targets OV5640; the recorded Plant 2 cloud boot log detected an OV3660. Check the detected sensor and board configuration rather than assuming all modules are identical.

The following steps prepare the **original local-hub firmware**. For direct cloud operation, follow the separate [cloud provisioning and OTA instructions](cloud/README.md#private-provisioning-and-camera-builds); the variants have different upload and photo-protection behavior.

**Preserve existing cards before using the original firmware.** Its SD initialization enables format-on-mount-failure and moves legacy flat `IMG_*.jpg` files into `DCIM/UNSORTED`; it also retains manual photo delete/erase controls. It does not provide the cloud build's SD-original protection. Use a spare card for local setup and retain an independent copy of irreplaceable originals.

1. Copy `ESP32_Station/secrets.example.h` to `ESP32_Station/secrets.h` and fill in your Wi-Fi and OTA credentials. The local file is excluded from Git.
2. Open `ESP32_Station/ESP32_Station.ino` in Arduino IDE. Select **AI Thinker ESP32-CAM**, **DIO** flash mode, install the tested **Espressif ESP32 core 3.3.11** and **ArduinoJson 7.4.3**. Keep the supplied `partitions.csv` beside the sketch; it defines two `0x1e0000`-byte OTA application slots.
3. Flash each board using a serial programmer or an already configured OTA connection. After serial flashing, disconnect GPIO0 from GND and restart the board.
4. Use the hub's **Device setup** panel to assign distinct Plant 1, Plant 2, and Plant 3 identities by current IP address. **Save to ESP & restart** sends the configuration and requests the necessary reboot. A fresh board defaults to Plant 1 until configured; confirm the MAC when distinguishing boards.
5. Enable the one-hour timer on each ESP's web page if needed. Fresh installations default to a disabled timer, a 3,600-second interval, and no time window. Confirm the clock before relying on dated filenames; the local firmware defaults to IST until its page supplies another offset. Take a manual photo to verify capture and SD storage.

The role migration itself changes only the saved sensor role and its migration marker; SD initialization has the separate legacy behavior described above. A second migration changes the old ten-second timer default to one hour once. OTA requires an awake, connected board and matching OTA credentials. See [setup and troubleshooting](docs/setup.md) for build commands and common failures.

Historical wiring identifies Plant 1 by ADS1115 on GPIO3/RX0 and GPIO1/TX0, Plant 2 by DHT22 data on GPIO13, and Plant 3 by its lack of an assigned external sensor. Confirm identity and MAC before each update. Working OTA firmware permits wireless updates; a fresh or unreachable board may require serial access.

## Optional photo analysis

In the original hub, analysis runs on the PC after an image is downloaded. Supply `OPENAI_API_KEY` in the hub environment or save a key through its dashboard to enable that integration. At the last recorded check on 22 October 2026, the separate cloud service had automatic analysis enabled for ordinary uploads using verified `gpt-6.1-sol` account access and one count check. Explicitly marked archive imports were excluded. On 23 October, read-only production records and the browser confirmed one newly uploaded Plant 2 photo and a completed report from 22 October, without submitting another request. This does not establish numerical measurement accuracy or capture/sleep reliability. Opting into firmware upload of old SD photographs does not itself mark them as archives: those ordinary uploads can trigger paid analysis. Fresh, unconfigured cloud installations default to analysis off; see [cloud analysis configuration](cloud/README.md#optional-analysis).

The local settings expose model, reasoning effort, and one to three tiller-count checks. New local databases default to `gpt-5.6-sol`, low effort and one check; those identifiers reflect the original development environment, so verify availability and reasoning support in your API account before enabling paid requests. Saving a key makes eligible queued photographs available for automatic paid analysis. By default, photos dated before the hub's first analysis cutoff, or without a recognized filename timestamp, synchronize as history. Reanalysis is an explicit action. Local analysis uses stored background responses and sends image content to the configured API.

Count checks occur within **one model response**, not separate independently sampled requests. Their selected length is enforced by the structured output schema. Outputs include confidence and notes; an obscured stem base or unreadable ruler may not support a reliable estimate. The project does not claim benchmarked counting accuracy or agronomic validation.

## Engineering decisions

- **Retain originals at the source.** Normal cloud uploads acknowledge files without removing the SD JPEGs. PC downtime does not itself remove originals; local firmware has the mount, migration and manual-deletion risks noted above.
- **Separate identity from address.** Station numbers persist while DHCP addresses can change. Discovery reconnects the two.
- **Record the work queue.** Downloads and analyses have explicit states, making interrupted work inspectable and recoverable.
- **Stage downloads before acceptance.** Images arrive in temporary files and are renamed only after validation.
- **Keep observation and interpretation distinct.** Photographs are primary records; model outputs are estimates that can be revisited.

The [engineering record](docs/engineering.md) explains recovery behavior, validation evidence, remaining failure windows, and useful next experiments.

## Validation

The development session on **14 September 2026** verified all three physical stations online with healthy camera and SD status, Camera-only profiles, and enabled 3,600-second timers. Manual captures from Plants 2 and 3 produced complete JPEGs of **302,654 bytes** and **177,926 bytes** respectively. Firmware compilation and packaged hub startup were also checked in that session.

The publication copy includes isolated tests for queue/schema defaults, manual capture routing, sidecar recovery, sensor-history association, truncated-image rejection, and duplicate-launch handling. Run them with:

```powershell
py -3 -m unittest discover -s PC_Hub -p "test_*.py" -v
node scripts/check-dashboard.cjs
```

The [source workflow](.github/workflows/checks.yml) is configured to run the hub tests, check dashboard JavaScript, and compile the firmware with example credentials. The badges link to workflow results; this documentation audit did not rerun builds or claim a new passing CI run. Historical hardware observations do not imply continuous uptime or current device availability.

Cloud validation also covers isolated server, import, backup, publication and restoration regressions, dashboard tests, and ESP32 compilation. Run the cloud suite after installing `cloud/server/requirements-dev.txt`:

```powershell
python -m pytest cloud/server/tests cloud/deploy/test_sd_import.py cloud/deploy/test_pull_camera.py cloud/review_tests -q
node --test cloud/server/tests/static_client.test.cjs
node --test cloud/review_tests/photo_capture_time.test.cjs
node --test cloud/review_tests/website_feedback.test.cjs
```

## Source map

```text
ESP32_Station/                 Camera firmware, SD storage, Wi-Fi, OTA, and timers
PC_Hub/Monitoring_Hub.py        Discovery, synchronization, persistence, and HTTP API
PC_Hub/static/index.html        Local dashboard and camera controls
PC_Hub/test_hub.py              Isolated tests with temporary storage
cloud/                         Separate cloud server, firmware, tests and deployment tools
docs/                          Architecture, setup, and engineering record
scripts/                       Source validation helpers
.github/workflows/             Automated checks
```

## Current limits

**The remote dashboard is designed for internet access over HTTPS** at [admin.riceplant.achantalabs.com](https://admin.riceplant.achantalabs.com), with password sessions and CSRF protection; cloud devices authenticate with individual credentials. The original PC hub and the ESPs' local HTTP pages lack that web-login protection and must stay on a private local network. The original hub listens on all local network interfaces; use appropriate firewall rules and do not expose these local services directly to the internet.

Neither variant automatically rotates old SD photographs to reclaim capacity. The local firmware refuses new writes below its one-MiB free-space margin and disables its running capture timer when full. Its mount-time formatting, legacy relocation and manual deletion controls differ from the protected cloud build. Comprehensive power-cut testing and labelled image-analysis evaluation remain future work. Cloud source marks ambiguous analysis submissions uncertain rather than automatically retrying a potentially paid request; confirm the deployed worker version before relying on that behavior. The original hub can resubmit when a request succeeds remotely before its response ID is persisted locally.

Diagnosis of Plant 2's reset-interrupted captures is deferred. The scheduled follow-up monitor was deleted at the user's request; no continuing investigation or scheduled monitoring is running.

Recorded source-only archives in Drive exclude credentials, photographs, databases and compiled firmware; they are not monitoring-data backups. The last checked cloud data backups remained on the same droplet, so a separate offsite data backup was still needed. Local SQLite backup rotation likewise does not back up the photographs. Preserve independent copies of originals; a diagnostic zero count does not establish missing photos.

## Development and attribution

This project was developed with AI-assisted implementation and debugging alongside hands-on board setup, flashing, and hardware validation. The repository documents the implemented system and its evidence; it does not imply that every line was written manually or that the system has demonstrated crop-yield improvements.

The firmware uses Espressif's Arduino ESP32 ecosystem and camera APIs. Dependencies retain their own licenses, and existing source notices are preserved. A repository-wide redistribution license has not yet been assigned pending an attribution review.
