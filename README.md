# Rice Plant Monitoring

**A three-camera monitoring system that keeps recording when the central computer is off.**

[![Source checks](https://github.com/mrsneaky29/Rice-Plant-Monitoring/actions/workflows/checks.yml/badge.svg)](https://github.com/mrsneaky29/Rice-Plant-Monitoring/actions/workflows/checks.yml)
![Hardware](https://img.shields.io/badge/hardware-ESP32--CAM-246b46)
![Hub](https://img.shields.io/badge/hub-Python%20%2B%20SQLite-346b9b)
![Deployment](https://img.shields.io/badge/deployment-local%20Wi--Fi-646a6b)

Rice growth is gradual; useful observations need to survive gaps in computer availability. This project connects three ESP32-CAM boards to a local PC hub so that each camera can record photographs to its own microSD card, while the PC handles synchronization, browsing, and optional image analysis whenever it is running.

The project grew through physical assembly, firmware flashing, live camera tests, network troubleshooting, and changes to how data is retained. It is a working prototype with documented limitations and a reproducible source build.

![System architecture](docs/architecture.svg)

## What happens in normal use?

1. Each configured station takes an hourly photograph and saves it locally.
2. The PC hub discovers cameras on the same Wi-Fi, including cameras whose IP addresses changed.
3. It downloads the photo history, alternating between stations, and records progress in SQLite.
4. The dashboard shows each plant's latest photograph, connection status, SD capacity, and available analysis history. **Take photo now** requests a fresh image from a specific camera.
5. When the PC stops, the dashboard becomes unavailable. The powered ESPs continue recording to SD and synchronize again when the hub returns.

All three stations currently use **Camera-only mode**. The earlier soil-moisture and DHT22 modules remain in the source but are disabled and ignored in this deployment. The boards stay powered; the hourly schedule controls capture, not deep sleep or power switching.

## Features

| Feature | Implementation |
| --- | --- |
| Independent recording | Camera and microSD storage on each ESP32-CAM |
| Persistent identities | `plant-1`, `plant-2`, and `plant-3`, with saved names and OTA hostnames |
| Automatic discovery | UDP announcements, startup broadcasts, and periodic rediscovery |
| Manual capture | Per-camera dashboard button that saves to SD and queues synchronization |
| Durable local storage | SQLite WAL transactions, staged `.part` downloads, and JPEG boundary checks |
| Recoverable analysis | Stored background response IDs, JSON result sidecars, and rolling database backups |
| Local dashboard | Gallery, queue, plant cards, storage, settings, and available analysis graphs |
| Optional image analysis | Structured tiller-count checks, growth-stage estimates, water-level estimates, and confidence fields |

## Run the PC hub

The hub runtime uses the **Python standard library**. Install Python 3.12 or newer and run this command from the repository root:

```powershell
py -3 PC_Hub/Monitoring_Hub.py --open-browser
```

Open **http://localhost:8765** on the host. Other devices on the same Wi-Fi can open `http://<PC-LAN-IP>:8765`. Allow the application's private-network firewall prompt so station discovery and local access can work.

On Windows, `PC_Hub/Open Monitoring Hub.bat` launches either the packaged executable or the Python source and opens the browser. The executable runs as a background web server; it does not create a separate desktop window. A second launch opens the existing dashboard instead of starting another hub.

The hub creates photographs, queue state, backups, and optional API-key files in **`PC_Hub/data/`**. That folder is ignored by Git. Preserve it when upgrading or moving your installation.

## Prepare the hardware

You need three AI Thinker ESP32-CAM boards with PSRAM, compatible camera modules, microSD cards, stable power, and a local Wi-Fi network. The development setup used OV5640 cameras; check `board_config.h` and the camera initialization settings against your hardware.

1. Copy `ESP32_Station/secrets.example.h` to `ESP32_Station/secrets.h` and fill in your Wi-Fi and OTA credentials. The local file is excluded from Git.
2. Open `ESP32_Station/ESP32_Station.ino` in Arduino IDE. Select **AI Thinker ESP32-CAM**, **DIO** flash mode, and install **ArduinoJson 7.4.3**. Keep the supplied `partitions.csv` beside the sketch; it defines two OTA application slots.
3. Flash each board using a serial programmer or an already configured OTA connection. After serial flashing, disconnect GPIO0 from GND and restart the board.
4. Use the hub's **Device setup** panel to assign distinct Plant 1, Plant 2, and Plant 3 identities by current IP address. Restart each board to apply its identity.
5. Enable the one-hour timer on each ESP's web page if needed. Fresh installations default to a disabled timer, with an interval of 3,600 seconds. Take a manual photo to verify capture and SD storage.

The firmware applies a one-time migration from existing sensor profiles to Camera-only while preserving Wi-Fi settings, identities, schedules, and SD photographs. OTA requires a running board connected to the network. See [setup and troubleshooting](docs/setup.md) for build commands and common failures.

## Optional photo analysis

The cameras do not send API requests. Analysis runs on the PC after an image is downloaded. Supply `OPENAI_API_KEY` in the hub environment or save a key through the dashboard to enable the existing integration.

The settings expose model, reasoning effort, and one to three tiller-count checks. Their configured model identifiers reflect the original development environment; verify availability and reasoning support in your API account before enabling paid requests. By default, photographs created before the hub's first analysis cutoff are synchronized as history rather than automatically analyzed. New photos are eligible, and reanalysis is an explicit action.

Count checks occur within **one model response**, not separate independently sampled requests. Their selected length is enforced by the structured output schema. Outputs include confidence and notes; an obscured stem base or unreadable ruler may not support a reliable estimate. The project does not claim benchmarked counting accuracy or agronomic validation.

## Engineering decisions

- **Retain originals at the source.** Every ESP keeps its photographs on SD, so PC downtime does not stop recording or remove the original data.
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

GitHub Actions runs the hub tests, checks dashboard JavaScript, and compiles the firmware with example credentials. Historical hardware observations do not imply continuous uptime or current device availability.

## Source map

```text
ESP32_Station/                 Camera firmware, SD storage, Wi-Fi, OTA, and timers
PC_Hub/Monitoring_Hub.py        Discovery, synchronization, persistence, and HTTP API
PC_Hub/static/index.html        Local dashboard and camera controls
PC_Hub/test_hub.py              Isolated tests with temporary storage
docs/                          Architecture, setup, and engineering record
scripts/                       Source validation helpers
.github/workflows/             Automated checks
```

## Current limits

This is a trusted-LAN prototype: the dashboard and device control endpoints do not have user authentication and should not be exposed to the public internet. SD storage is retained until capacity is exhausted. Comprehensive power-cut testing and labelled image-analysis evaluation remain future work. A crash between remote API submission and persistence of its response ID can still lead to a duplicate submission.

## Development and attribution

This project was developed with AI-assisted implementation and debugging alongside hands-on board setup, flashing, and hardware validation. The repository documents the implemented system and its evidence; it does not imply that every line was written manually or that the system has demonstrated crop-yield improvements.

The firmware uses Espressif's Arduino ESP32 ecosystem and camera APIs. Dependencies retain their own licenses, and existing source notices are preserved. A repository-wide redistribution license has not yet been assigned pending an attribution review.
