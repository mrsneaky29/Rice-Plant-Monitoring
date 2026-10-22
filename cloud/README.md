# Rice Cloud

Private camera dashboard at **https://admin.riceplant.achantalabs.com**. This variant supports three ESP32-CAM stations communicating directly with the cloud; Plant 2 is verified, and Plants 1 and 3 await rollout. A PC bridge is not required. The original local hub and firmware remain separate.

## Operation

Sleep mode is off until explicitly enabled. With sleep enabled, the station wakes on its photo schedule, reads pending cloud commands, saves a due JPEG to SD, uploads it with a checksum, and returns to deep sleep. Failed uploads remain on SD and retry later. Photos are never automatically deleted from the card.

The dashboard queues **Take photo**, **Stay awake**, **Resume sleep**, and schedule changes. A deeply sleeping station cannot receive commands until its next wake. **Press its physical RESET button** for immediate local wake: reset/power-on starts a ten-minute maintenance session. Stay awake extends that session until Resume sleep or reboot. Local OTA remains available while awake.

Resume sleep clears the temporary Stay awake override; it does not enable a disabled sleep setting. Disabling the photo timer also prevents automatic deep sleep. The dashboard accepts intervals from one hour to seven days and local-time windows, including windows crossing midnight. Changes take effect when the board receives and reports them, not when the button is pressed. Full physical sleep/wake acceptance is pending.

The dashboard distinguishes requested settings from settings reported by the station. A blank station has not connected yet; old imported status is historical and does not prove current connectivity. Observation photos and AI estimates are shown separately. The initial deployment had analysis disabled; the configured live service enables it for new ordinary uploads as of 22 October 2026.

Photo cards show complete images at their natural aspect ratio, including on mobile. Hovering does not zoom or crop them; enlargement remains optional.

Normal builds upload **new captures only**. Existing SD photographs remain in place for collection through an SD reader. A new capture is written to a `.part` file, closed, and published as a JPEG after an `.upload` intent is created; a `.cloud` sidecar acknowledges successful upload without deleting the original. Photo delete/erase controls are blocked, and mounting never formats a failing card. Uploading historical photos requires an explicit `--upload-existing-photos` build option and the matching OTA option; leave both off while preserving an existing archive.

## Verified deployment — 18 October 2026

Plant 2 received protected normal firmware via OTA. Camera initialization, SD mounting, cloud command acknowledgement, three fresh captures and direct uploads were verified. The first uploaded JPEG's checksum matched its stored cloud file. Automatic dashboard refresh was observed, and the live photo display was updated independently of the server release.

At the 18 October checkpoint, Plants 1 and 3 had not completed this cloud firmware rollout, Plant 2 sleep was off, and a full physical sleep/wake cycle was unverified. A local status request timed out during capture/upload activity while cloud reports continued. Existing SD photos were counted without importing their contents; their file-by-file integrity was not established. See the 22 October follow-up below for the later live settings and unresolved capture failures.

The backend release checked on 18 October was `20261017-cloud-v3`. Later server and backup improvements were not deployed during that protected firmware restoration; that day's website update was limited to photo-display CSS. The 22 October follow-up describes subsequent targeted changes to this same release directory. Compiling source or passing tests does not prove a live deployment or hardware acceptance.

## Live follow-up — 22 October 2026

The dashboard now displays the capture timestamp encoded in each camera filename, with camera-local clock components preserved. Gallery ordering, date filters and latest-photo selection use capture dates rather than upload arrival dates. Upload time remains separately labeled in the enlarged-photo caption and gallery tooltip. Photos without a valid timestamp show "Capture time unavailable" instead of substituting an upload time.

Automatic analysis is enabled in the live private environment using verified account access to `gpt-6.1-sol`, one verification pass per new ordinary upload. The deployed backend received the archive-import exclusion and missing startup import fix, alongside the tested worker. No historical jobs were queued. A new successful camera upload is still required to verify a complete paid analysis/report cycle.

Release checks passed 89 isolated Python tests and 13 JavaScript checks, including delayed uploads, capture-date filtering, unknown timestamps, secure activation, enabled startup and duplicate/archive queue safety. The published dashboard assets matched their tested hashes and the service health check passed.

Plant 2 currently reports sleep enabled with a one-hour interval. Read-only observation found daytime photo gaps; two user-issued capture commands returned `uncertain` with `reset_during_command`, without a new cloud photo. No camera firmware or settings were changed in this follow-up. A scheduled wake/capture/upload/sleep cycle and the cause of these resets remain under investigation. The older 18 October results above are a dated checkpoint, not proof of current reliability.

## Layout

- `server/`: FastAPI service, SQLite metadata, password sessions, station-bound credentials, validated uploads, durable commands, optional analysis and dashboard.
- `firmware/ESP32_Station/`: separate Arduino firmware, shared SD locking, RTC scheduling, cloud journals and sleep controls.
- `deploy/`: isolated system service/Caddy configuration, backup/restore validation, packaging, private provisioning and OTA helpers.
- `API_CONTRACT.md`: request shapes, settings and authentication boundaries.

## Local testing

Use Python 3.12 or newer and Node.js. From this directory:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r server\requirements-dev.txt
.\.venv\Scripts\python.exe -m pytest server\tests deploy\test_sd_import.py deploy\test_pull_camera.py review_tests -q
node --test server\tests\static_client.test.cjs
node --test review_tests\photo_capture_time.test.cjs
.\.venv\Scripts\python.exe deploy\preview.py
```

The preview uses an isolated `.private/preview-data` directory and listens only on `127.0.0.1:8796`. Its password is `Local-preview-only-2026`; this is a QA fixture, never a production credential. Production refuses to start without a valid administrator password hash.

## Private provisioning and camera builds

`Configure-Private.ps1` prompts locally for the dashboard password, downloads the public ISRG Root X1 CA over verified HTTPS, and creates three independent camera credentials under ignored `.private/`. Never share those files. To configure a different deployment, update the service hostname and trusted CA together.

Keep the existing Wi-Fi and OTA settings in ignored `firmware/ESP32_Station/secrets.h`. Use the Arduino CLI supplied by your Arduino IDE:

For a fresh source checkout, first create the Python environment from Local testing above, install Arduino ESP32 core **3.3.11** and **ArduinoJson 7.4.3**, and copy `firmware/ESP32_Station/secrets.example.h` to the ignored `secrets.h`. Configure each board's saved station identity before building/flashing its matching token. The build helper does not assign the board's NVS identity. Do not rerun provisioning expecting credential rotation: existing configuration is preserved.

```powershell
.\.venv\Scripts\python.exe deploy\build_firmware.py plant-2 --cli "C:\path\to\arduino-cli.exe"
.\.venv\Scripts\python.exe deploy\flash_ota.py plant-2 192.168.1.204 --espota "C:\path\to\esp32\tools\espota.py"
```

Build one image per station because its bearer credential is bound to that identity. Private images and manifests stay under `.private/images`; the OTA helper checks the live station identity, image checksum and 1,966,080-byte partition limit before transfer. The existing OTA password is read locally and never passed in a command-line argument. Rediscover addresses before flashing; the example address is not permanent.

For an additional identity check, pass `--expected-mac "AA:BB:CC:DD:EE:FF"` with the verified hardware MAC. The OTA helper also checks the historical-upload policy and requires photo protection in a normal image's manifest. `--status-only` builds a separate diagnostic image that leaves SD unmounted and disables photo uploads and cloud commands; its reported zero photo count does not establish an empty card.

Update one board first, verify camera/SD/cloud operation, then test a complete scheduled sleep/wake cycle and physical RESET before extending rollout. Reverting requires an awake board or serial access; keep the previous firmware image available. Physical current measurement is required before claiming a particular power saving. Sensor wiring and defaults are described in the [firmware README](firmware/ESP32_Station/README.md).

## Droplet installation

The rice service uses `/opt/rice-cloud`, `/var/lib/rice-cloud`, `/etc/rice-cloud/server.env`, and loopback port `8796`. It has its own `rice-cloud` system user. Vivayu remains on its existing user, storage, domains and port.

Set the `admin.riceplant` A record to the droplet, provision a private root-readable `server.env`, package with `deploy/package.py --server-only`, and run `deploy/install.sh ARCHIVE RELEASE_ID` on the droplet. The installer adds an independently managed Caddy import, validates before reload, verifies health and a backup restore, and rolls back on failure. Inspect resource availability and existing hostname ownership before installation.

Supply device provisioning JSON privately on stdin to `deploy/provision_devices.py`; only token hashes are stored in the database. No credentials belong in Caddy, Git, public archives or deployment command arguments.

The daily systemd backup creates a consistent SQLite snapshot plus referenced photo files and verifies their restoration/checksums. Backups are local to the droplet under `/var/backups/rice-cloud`; a droplet loss also loses those copies. Source ZIP/Git backups in Drive do not contain monitoring photos or credentials. An offsite monitoring-data backup remains a separate configuration task.

In the DigitalOcean droplet Console, `journalctl -u rice-cloud.service -f` follows server logs. Device boot and detailed camera logs remain available over local serial/RAM-log interfaces; server logs record cloud requests rather than the complete device log.

## Existing photos and analysis

`deploy/prepare_import.py HUB_DATA` opens the original local SQLite database read-only, creates a consistent snapshot, strips credential-like settings and packages JPEG copies for private migration. `server/cli.py import-legacy --db SNAPSHOT --photos PHOTO_ROOT` imports files and historical analysis without changing originals or triggering paid requests. Repeated imports preserve matching data and reject conflicting image contents.

Additional camera SD photos can be inventoried/imported using `deploy/import_sd.py` when a reader is available. Keep each card associated with its station; do not merge two cards under the same camera identity. Power off a camera before removing its card.

For a read-only inventory, run `python deploy/import_sd.py plant-2 "E:\DCIM"` with the actual reader's DCIM folder. Upload is an additional explicit `--upload` option, which is not part of the current SD retrieval plan. The original files remain on the card; keep a separate verified copy before any future cleanup.

## Optional analysis

The configured live service has automatic analysis on for new ordinary uploads. A fresh, unconfigured installation defaults to off until a supported model, provider key and desired count checks are configured in the private server environment and the service is restarted. Historical import does not enqueue analysis. The single-process worker stores background response IDs, resumes polling after restarts, and marks ambiguous submissions uncertain rather than automatically resending a potentially paid request. Count checks within one response are not independent model samples, and estimates are not agronomically validated.

Private environment fields are `RICE_OPENAI_API_KEY`, `RICE_ANALYSIS_MODEL`, `RICE_ANALYSIS_PASSES` (1–3) and `RICE_ANALYSIS_ENABLED`. The requested model is `gpt-6.1-sol`; account access was verified before live activation. For a fresh setup, `Configure-Analysis.ps1` opens a hidden key prompt and checks model access without submitting an analysis. The equivalent command is `python deploy/configure_analysis.py --prompt-key`. It prepares ignored local configuration only. `python deploy/activate_analysis.py` securely forwards only the four analysis settings through SSH input, checks the deployed archive exclusion and absence of outstanding jobs, retains a private rollback copy, and restarts only Rice Cloud. It refuses repeat activation while analysis is already on. The configured live service stays enabled across restarts; a fresh unconfigured installation remains disabled until a key and model are deliberately configured. Never put a key in Git, browser code or a command-line argument.

## Troubleshooting and acceptance

| Symptom | Interpretation or next check |
| --- | --- |
| Old LAN address stops responding | DHCP may have changed; discover the station and verify identity/MAC before acting |
| Cloud heartbeat arrives but local status times out | Capture/upload or SD contention can delay local HTTP; avoid repeated probes or resetting during a write |
| Diagnostic firmware reports zero photos | SD is deliberately unmounted; this does not establish an empty or damaged card |
| Take photo remains pending | Sleeping boards wait until the next wake; diagnostic firmware deliberately ignores commands |
| Capture completed but image not visible yet | Command acknowledgement and photo upload are separate; wait for an accepted upload and dashboard refresh |
| A request disappears from the pending queue without a photo | Disappearance is not proof of success; inspect its completed, failed or uncertain result. A reset during a started capture is reported as uncertain and is not blindly repeated |
| Photo upload warning remains after recovery | The present dashboard can retain a warning from the error counter since reboot, even when the latest error is empty; this display defect remains unresolved |
| SD fails to mount or fills | Preserve originals and inspect through a reader; no automatic formatting or photo deletion occurs |
| USB power is present but no serial port appears | A charging connection alone does not provide a USB-to-serial interface |

Software checks on 18 October passed 78 isolated Python tests, 7 dashboard tests and 6 original hub tests, plus ESP32 compile-time checks and a normal firmware build. These do not replace power-cut, weak-Wi-Fi, file-by-file SD integrity, or physical sleep/wake acceptance. The hardened backup code is in source; confirm the deployed release before relying on a particular fix. `python deploy/backup.py --verify ARCHIVE.tar.gz` validates a restore in temporary storage and does not replace the live database.

## Publication boundaries

Source archives exclude `.private`, virtual environments, live data, Wi-Fi/OTA secrets, device tokens and compiled binaries. Commit only the sanitized source copy. Preserve original local software and synced `sources/` references.
