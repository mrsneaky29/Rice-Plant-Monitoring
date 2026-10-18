# Rice Cloud

Private camera dashboard at **https://admin.riceplant.achantalabs.com**. Three ESP32-CAM stations communicate directly with the cloud; no PC bridge is needed. The original local hub and firmware remain separate and unchanged.

## Operation

Sleep mode is off until explicitly enabled. With sleep enabled, the station wakes on its photo schedule, reads pending cloud commands, saves a due JPEG to SD, uploads it with a checksum, and returns to deep sleep. Failed uploads remain on SD and retry later. Photos are never automatically deleted from the card.

The dashboard queues **Take photo**, **Stay awake**, **Resume sleep**, and schedule changes. A deeply sleeping station cannot receive commands until its next wake. **Press its physical RESET button** for immediate local wake: reset/power-on starts a ten-minute maintenance session. Stay awake extends that session until Resume sleep or reboot. Local OTA remains available while awake.

The dashboard distinguishes requested settings from settings reported by the station. A blank station has not connected yet; old imported status is historical and does not prove current connectivity. Observation photos and AI estimates are shown separately. Analysis is disabled in the initial deployment.

Photo cards show complete images at their natural aspect ratio, including on mobile. Hovering does not zoom or crop them; enlargement remains optional.

Normal builds upload **new captures only**. Existing SD photographs remain in place for collection through an SD reader. A new capture is written to a `.part` file, closed, and published as a JPEG after an `.upload` intent is created; a `.cloud` sidecar acknowledges successful upload without deleting the original. Photo delete/erase controls are blocked, and mounting never formats a failing card. Uploading historical photos requires an explicit `--upload-existing-photos` build option and the matching OTA option; leave both off while preserving an existing archive.

## Verified deployment — 18 October 2026

Plant 2 received protected normal firmware via OTA. Camera initialization, SD mounting, cloud command acknowledgement, three fresh captures and direct uploads were verified. The first uploaded JPEG's checksum matched its stored cloud file. Automatic dashboard refresh was observed, and the live photo display was updated independently of the server release.

Plants 1 and 3 have not completed this cloud firmware rollout. Plant 2 sleep remains off, and a full physical sleep/wake cycle is still unverified. A local status request timed out during capture/upload activity while cloud reports continued; local responsiveness under load needs further testing. Existing SD photos were counted without importing their contents, but their file-by-file integrity has not been established.

The repository includes server and backup improvements that may be newer than the deployed server release. Compiling source or passing tests does not prove a live deployment or hardware acceptance.

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
.\.venv\Scripts\python.exe deploy\preview.py
```

The preview uses an isolated `.private/preview-data` directory and listens only on `127.0.0.1:8796`. Its password is `Local-preview-only-2026`; this is a QA fixture, never a production credential. Production refuses to start without a valid administrator password hash.

## Private provisioning and camera builds

`Configure-Private.ps1` prompts locally for the dashboard password, downloads the public ISRG Root X1 CA over verified HTTPS, and creates three independent camera credentials under ignored `.private/`. Never share those files. To configure a different deployment, update the service hostname and trusted CA together.

Keep the existing Wi-Fi and OTA settings in ignored `firmware/ESP32_Station/secrets.h`. Use the Arduino CLI supplied by your Arduino IDE:

```powershell
.\.venv\Scripts\python.exe deploy\build_firmware.py plant-2 --cli "C:\path\to\arduino-cli.exe"
.\.venv\Scripts\python.exe deploy\flash_ota.py plant-2 192.168.1.204 --espota "C:\path\to\esp32\tools\espota.py"
```

Build one image per station because its bearer credential is bound to that identity. Private images and manifests stay under `.private/images`; the OTA helper checks the live station identity, image checksum and 1,966,080-byte partition limit before transfer. The existing OTA password is read locally and never passed in a command-line argument. Rediscover addresses before flashing; the example address is not permanent.

For an additional identity check, pass `--expected-mac "AA:BB:CC:DD:EE:FF"` with the verified hardware MAC. The OTA helper also checks the historical-upload policy and requires photo protection in a normal image's manifest. `--status-only` builds a separate diagnostic image that leaves SD unmounted and disables photo uploads and cloud commands; its reported zero photo count does not establish an empty card.

Update one board first, verify camera/SD/cloud operation, then test a short sleep interval and physical RESET before updating the others. Reverting requires an awake board or serial access; keep the previous firmware image available. Physical current measurement is required before claiming a particular power saving.

## Droplet installation

The rice service uses `/opt/rice-cloud`, `/var/lib/rice-cloud`, `/etc/rice-cloud/server.env`, and loopback port `8796`. It has its own `rice-cloud` system user. Vivayu remains on its existing user, storage, domains and port.

Set the `admin.riceplant` A record to the droplet, provision a private root-readable `server.env`, package with `deploy/package.py --server-only`, and run `deploy/install.sh ARCHIVE RELEASE_ID` on the droplet. The installer adds an independently managed Caddy import, validates before reload, verifies health and a backup restore, and rolls back on failure. Inspect resource availability and existing hostname ownership before installation.

Supply device provisioning JSON privately on stdin to `deploy/provision_devices.py`; only token hashes are stored in the database. No credentials belong in Caddy, Git, public archives or deployment command arguments.

The daily systemd backup creates a consistent SQLite snapshot plus referenced photo files and verifies their restoration/checksums. Backups are local to the droplet under `/var/backups/rice-cloud`; a droplet loss also loses those copies. Source ZIP/Git backups in Drive do not contain monitoring photos or credentials. An offsite monitoring-data backup remains a separate configuration task.

In the DigitalOcean droplet Console, `journalctl -u rice-cloud.service -f` follows server logs. Device boot and detailed camera logs remain available over local serial/RAM-log interfaces; server logs record cloud requests rather than the complete device log.

## Existing photos and analysis

`deploy/prepare_import.py HUB_DATA` opens the original local SQLite database read-only, creates a consistent snapshot, strips credential-like settings and packages JPEG copies for private migration. `server/cli.py import-legacy --db SNAPSHOT --photos PHOTO_ROOT` imports files and historical analysis without changing originals or triggering paid requests. Repeated imports preserve matching data and reject conflicting image contents.

Additional camera SD photos can be inventoried/imported using `deploy/import_sd.py` when a reader is available. Keep each card associated with its station; do not merge two cards under the same camera identity. Power off a camera before removing its card.

## Optional analysis

Analysis is disabled by default. Enable it only after configuring a supported model, provider key and desired count checks in the private server environment; restart the service. New uploads can then enqueue work. Historical import does not enqueue analysis. The single-process worker stores background response IDs, resumes polling after restarts, and marks ambiguous submissions uncertain rather than automatically resending a potentially paid request. Count checks within one response are not independent model samples, and estimates are not agronomically validated.

## Publication boundaries

Source archives exclude `.private`, virtual environments, live data, Wi-Fi/OTA secrets, device tokens and compiled binaries. Commit only the sanitized source copy. Preserve original local software and synced `sources/` references.
