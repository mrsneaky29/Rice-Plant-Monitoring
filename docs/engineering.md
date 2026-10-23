# Engineering record

This page records the **original local-hub implementation** and its historical tests. The current remote-access path is the [public HTTPS cloud dashboard](https://admin.riceplant.achantalabs.com), described in the [cloud README](../cloud/README.md). Local HTTP/UDP behavior below does not describe the cloud service's authentication or deep-sleep lifecycle.

## Problem and design

The central PC is not available continuously. Putting storage on each camera allows observations to survive PC downtime. The hub retrieves the backlog when it returns; keeping originals on SD makes the devices a second source of recovery.

UDP announcements carry station identity and IP. Stations rotate their scheduled announcements in 20-minute slots, with staggered fallback attempts. The hub also broadcasts at startup and every minute. Device identity is persistent, but IP addresses are not: the prototype demonstrated a station moving to a different DHCP address. Each board needs a unique station number, even after flashing the same sketch.

With the original local firmware, devices stay powered and run their normal loops. Its capture timer does not implement deep sleep. The separate cloud firmware supports optional timer wake from deep sleep; dashboard commands wait until the next check-in or wake. Neither variant can remotely turn on a physically powered-off board.

## Queue and recovery

The primary image key is `(station_id, date, filename)`. Photo synchronization alternates stations and orders a station's photos by date and filename. A download is staged as `.part`, checked for expected size and JPEG boundary markers, then renamed into place. SQLite uses WAL mode and full synchronization. Analyses have JSON sidecars; the database can reconstruct completed results from those files. Backups are produced at startup, shutdown, and hourly, retaining seven files.

Submitted background response IDs support later retrieval. A crash between a successful remote submission and persistence of its response ID remains a possible duplicate-submission window. This is not a guarantee of exactly-once API execution. Exhaustive physical power-cut testing has not been documented.

## Observed hardware validation

The development session on **14 September 2026** recorded:

| Check          | Recorded observation                                                              |
| -------------- | --------------------------------------------------------------------------------- |
| All stations   | Online, SD mounted, camera initialized, Camera-only profile, hourly timer enabled |
| Plant 1        | A complete JPEG retrieved from the latest-frame endpoint                          |
| Plant 2        | Manual capture saved successfully; JPEG size 302,654 bytes                        |
| Plant 3        | Manual capture saved successfully; JPEG size 177,926 bytes                        |
| Firmware build | 1,451,592 bytes of program storage; 99,052 bytes of global data in that build     |
| Hub            | Tests, JavaScript syntax check, and packaged executable startup passed            |

These measurements come from a previous development run. They do not establish long-term stability or inference accuracy. Fresh board flashing may preserve NVS settings; station role and identity must be checked after reboot. A 404 from `/latest.jpg` immediately after reboot can mean no post-reboot photo exists; `/snap` is the capture test.

## Changes in the publication copy

Wi-Fi and OTA credentials moved to an ignored `secrets.h`. Hub API keys can also come from the environment. Launching a second hub opens the existing dashboard before starting more workers or touching the database. JPEG validation rejects missing end markers. The original hub runs in Camera-only mode. Cloud automatic analysis was enabled for new ordinary uploads at the 22 October 2026 checkpoint; a complete fresh-photo analysis cycle was still unverified then.

## Limitations and next experiments

- Evaluate tiller estimates against human-labelled photographs, including occlusion and multiple clumps.
- Repeated counting checks currently happen inside one model response; independent requests need a separate experiment and cost accounting.
- Add reproducible power-loss fault injection across queue transitions.
- Keep the original hub and camera HTTP controls on the local network. Use the separate authenticated HTTPS cloud service for internet access; it already implements administrator sessions, CSRF checks and station-bound credentials.
- Improve station discovery diagnostics and configure a reproducible time-zone policy. Firmware currently defaults to UTC+05:30.
- Monitor finite SD capacity and preserve a separate backup. The original local firmware permits delete/erase operations, requests formatting on SD mount failure, and relocates legacy flat photos. The cloud protected build disables these automatic/destructive paths; do not use the original variant as a recovery image for a card containing the only copy of photographs.
- Review inherited firmware attribution before selecting a redistribution license.

## Portfolio interpretation

This project offers concrete material for discussing embedded systems, networking, state recovery, and experimental limitations. The strongest evidence is the three-board prototype and its documented capture tests. Code size is not a measure of originality or impact. AI tools assisted implementation and debugging; no claim is made that every line was written manually or that the system improves crop yields.
