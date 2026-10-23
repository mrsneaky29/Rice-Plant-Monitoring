# Original local-hub station firmware

Current remote access uses the password-protected **[HTTPS cloud dashboard](https://admin.riceplant.achantalabs.com)** and the separate [cloud firmware](../cloud/firmware/ESP32_Station/README.md), which supports direct uploads, queued controls and optional deep sleep without a PC bridge.

This directory contains the original local-hub firmware for three AI Thinker ESP32-CAM stations. Its local HTTP controls and SD behavior differ from the cloud build. At the last recorded check on 22 October 2026, Plant 2 had received the cloud variant, Plants 1 and 3 awaited rollout, and reliable hourly capture/upload/sleep remained unverified. This 23 October documentation audit did not check live devices or change firmware.

**Use a spare card for this local variant if originals must be protected.** Its SD initialization enables formatting on mount failure and moves legacy flat `DCIM/IMG_*.jpg` files into `DCIM/UNSORTED`. Manual photo deletion and erase controls also remain available. The cloud build disables those behaviors by default; do not assume its protection applies to this directory.

## Profiles

- Fresh boards default to station 1 and role `none` — Camera-only. Assign unique identities before using three boards together.
- A one-time `cameraOnlyV1` migration selects Camera-only while retaining Wi-Fi,
  identities, schedules and SD photographs. Later explicit `soil` and `dht22`
  profile selections remain supported by the firmware; check reported `sensor_role` rather than inferring the active profile from wiring.
- The stock PC hub sets `CAMERA_ONLY_MODE = True`: it ignores external readings and changes discovered `soil` or `dht22` profiles back to `none`, requesting a reboot. Explicit firmware sensor selection alone will not enable sensor operation with that hub unchanged.

The station number, display name, role, OTA hostname, and web hostname persist
after restart. Use the PC dashboard's **Device setup → Save to ESP & restart** to configure each board by its current IP address and request the required reboot. Confirm its MAC before updating it. The hostnames are `ricecam-1.local`,
`ricecam-2.local`, and `ricecam-3.local`.

## Offline behavior

Enabled camera timers do not depend on the PC. With working camera, SD storage
and power, a station continues saving photographs while the PC is unavailable.
Fresh installations default to a disabled timer with a 3,600-second interval;
the time window is disabled. A one-time migration changes the former ten-second default to one hour. Changing the interval or enabling the timer restarts its countdown; captures are interval-based rather than guaranteed at the top of each hour. An enabled window suppresses captures outside it and holds them while the clock is unset. The firmware defaults to IST (+05:30); NTP sets UTC and the camera page can supply a different local offset.

This variant stays powered between captures and does not implement deep sleep. Sensor recording is off in Camera-only mode. For explicit sensor roles, boot enables a twenty-minute recording interval; saved DHT interval/enable fields are not the boot defaults. The stock hub will override those roles as described above.

When the PC returns, UDP discovery on port 4210 lets the hub reconnect and retrieve the backlog over local HTTP. Synchronization does not delete SD originals. No automatic oldest-photo rotation exists: writes stop below a one-MiB free-space margin, and the capture timer disables itself when full. SD retention is subject to the local formatting, relocation and manual-deletion behavior noted above.

## Build

- Sketch: `ESP32_Station.ino`
- Board: **AI Thinker ESP32-CAM**
- Flash mode: **DIO**
- Tested ESP32 Arduino core: **3.3.11**, as pinned in the source-check workflow
- Partition scheme: use the project's `partitions.csv` (two `0x1e0000`-byte OTA app slots)
- Library: **ArduinoJson 7.4.3**
- Copy `secrets.example.h` to `secrets.h` before compiling and fill in your credentials.

For serial flashing, disconnect GPIO0 from GND before normal boot. OTA requires an awake board, network reachability and the matching OTA password; the source workflow's compilation is not proof that a physical board has been updated. See [local setup](../docs/setup.md) for build steps. The configured sensor target is OV5640, while `camera_setup.cpp` selects capture/stream settings from the detected sensor PID; check the boot log for the actual module.

## Wiring

The historical wiring uses GPIO3/RX0 as ADS1115 SDA and GPIO1/TX0 as SCL on Plant 1. Disconnect those lines while using a serial programmer. Plant 2 uses GPIO13 for DHT22 data. Plant 3 has no assigned external sensor. The selected role determines which peripheral code runs; wiring alone does not enable it.

## Analysis and local access

Boot disables automatic on-device AI and clears its saved enable flag. The original HTTP API still contains a switch to enable it and a manual photo-analysis route, and `water_level.cpp` retains the request implementation; therefore this variant does not guarantee that an explicitly invoked device-side request is impossible. That legacy implementation disables TLS certificate verification. Use the PC hub's optional analysis after copying photographs, or the cloud's server-side worker, rather than treating the legacy device integration as the current analysis path.

At the last recorded cloud check on 22 October 2026, server-side analysis was enabled for ordinary uploads with `gpt-6.1-sol` and one count check; explicitly marked archive imports were excluded. A later read-only verification on 23 October found one newly uploaded Plant 2 photo and its completed server-side report from 22 October; the physical sleep cycle and capture reliability remain unverified. Opting into cloud firmware upload of old SD photographs does not itself mark those uploads as archives, so they can trigger paid analysis. Source defaults and these dated deployment settings are separate from current hardware acceptance.

This variant's local camera HTTP page and the original PC hub lack web-login protection and must stay on a private local network. Do not expose those local services directly to the internet. Remote access uses the separate HTTPS cloud dashboard with password sessions and CSRF protection and per-device upload credentials.
