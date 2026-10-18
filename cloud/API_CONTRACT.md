# Rice Cloud API contract

This document records the current contract shared by the Rice Cloud server, the ESP32-CAM firmware, and the browser dashboard. It reflects the backend and firmware implementation checked on 2026-10-17.

## Admin browser session

The login shell and static assets are public; application data and photos require an admin session.

| Request | Behavior |
|---|---|
| `GET /api/session` | Returns `{authenticated, csrf_token}`. An unauthenticated session returns `authenticated: false` and a null token. |
| `POST /api/login` | JSON `{password}`. On success returns `{authenticated: true, csrf_token}` and sets the HTTP-only `rice_admin_session` cookie. |
| `POST /api/logout` | Requires the session cookie, same-origin `Origin`, and `X-CSRF-Token`; deletes the session and cookie. |

All state-changing browser requests require `X-CSRF-Token` and the same-origin `Origin`. FastAPI errors use a `detail` field; the dashboard displays that detail when a request fails. The default session lifetime is 12 hours. Cookie security is configurable for local development and is enabled by default.

## Dashboard state and photos

`GET /api/state` requires an admin session and returns:

```json
{
  "stations": [
    {
      "station_id": "plant-1",
      "name": "Plant 1",
      "last_seen": 1792200000000,
      "next_wake_at": 1792221600000,
      "sleep_enabled": true,
      "awake": false,
      "status": {},
      "last_capture": null,
      "latest_photo": {"date": "2026-10-17", "filename": "photo.jpg", "uploaded_at": 1792200000000},
      "pending_commands": [{"id": "…", "type": "take_photo", "payload": {}}]
    }
  ],
  "photos": [
    {"id": 1, "station_id": "plant-1", "date": "2026-10-17", "filename": "photo.jpg", "url": "/api/photos/1", "uploaded_at": 1792200000000}
  ],
  "analysis_enabled": false
}
```

Admin-state timestamps are Unix epoch milliseconds for JavaScript `Date`. Device sync timestamps use Unix epoch seconds. `latest_photo` currently carries its date, filename, and upload time but no photo ID or URL; the dashboard joins it to the `photos` array by station ID, date, and filename. Photo URLs in the list point to `GET /api/photos/{photo_id}`, which is also session-protected.

`status` is the device-reported JSON object and is passed through by the server. Current firmware reports `cloud_enabled`, `sleep_enabled`, `awake`, `maintenance_remaining_seconds`, `upload_error_count`, `last_upload_error`, `timer_enabled`, `interval_seconds`, `window_enabled`, `window_start`, `window_end`, `in_window`, `scheduled_due`, `time_set`, `next_wake_at`, `wifi_connected`, `ip`, `sd_mounted`, `sd_full`, `photo_count`, and `camera_ok`. Window bounds in device status are minute-of-day integers. The top-level `sleep_enabled`, `awake`, and `next_wake_at` fields are also sent on sync for the server's station summary.

## Command queue

`POST /api/stations/{station_id}/commands` requires an admin session, same-origin `Origin`, and `X-CSRF-Token`. The JSON body is `{ "type": "…", "payload": {} }`. The server accepts these command types:

| Type | Payload | Firmware behavior |
|---|---|---|
| `take_photo` | `{}` | Capture once on the next command sync. |
| `stay_awake` | `{}` | Keep the device awake until `resume_sleep` or reboot. |
| `resume_sleep` | `{}` | End the stay-awake session and resume the configured sleep behavior. |
| `set_settings` | See below. | Apply settings on device sync and save timer settings to NVS. |

`set_settings` requires exactly these fields:

```json
{
  "sleep_enabled": true,
  "timer_enabled": true,
  "interval_seconds": 21600,
  "window_enabled": false,
  "window_start": "06:00",
  "window_end": "18:00"
}
```

The three `*_enabled` fields are booleans. `interval_seconds` is an integer from 3,600 through 604,800 inclusive (1–168 hours). Window bounds are `HH:MM` strings using 24-hour local station time. The firmware also accepts those bounds internally as minutes 0–1,439; the admin API requires strings. Wrapped windows across midnight are supported by the timer. The dashboard sends this complete payload and queues it for each station currently returned by `/api/state`.

The server keeps at most eight pending commands per station. Successful creation returns the command ID and queued payload; commands are delivered on a station sync, not immediately while it sleeps. The dashboard labels a requested setting as applied only after the device reports it in a later sync.

`POST /api/stations/{station_id}/commands/{command_id}/cancel` cancels only pending commands. It returns `{ "ok": true }`; a missing command returns 404, and a command that is no longer pending returns 409.

## Device sync and photo upload

Devices authenticate with their station-specific bearer token. The server rejects a station ID that does not match the token.

| Request | Contract |
|---|---|
| `POST /api/device/sync` | JSON `{station_id, status, next_wake_at, sleep_enabled, awake, last_capture}`. The server records the report and returns `{ok, server_time, commands}`. `server_time` and device wake timestamps are epoch seconds. |
| `GET /api/device/commands` | Returns the station's pending commands. |
| `POST /api/device/commands/{command_id}/result` | JSON `{state, message?, photo?}`. State is `completed`, `failed`, or `uncertain`; `photo`, when present, contains a valid `{date, filename}` identity. |
| `PUT /api/device/photos/{date}/{filename}` | Uploads a complete JPEG with `X-Content-SHA256`; maximum size is 16 MiB. |

When configured for deep sleep, a station wakes on its timer to sync commands, save a due capture, upload pending photos, and sleep again. Power-on or external ESP RESET starts a ten-minute local maintenance window with the local page and OTA available. Cloud `stay_awake` lasts until `resume_sleep` or reboot. A reset wakes the station immediately; queued cloud commands are still processed through a sync.

## Photo analysis and imported history

`GET /api/analyses` requires an admin session and returns `{analyses, analysis_enabled}`. Each item contains the record ID, source table/key, station ID, date, filename, and the record object. Historical records imported from the former local hub are preserved and marked with an imported archive badge.

`GET /api/analysis/jobs` requires an admin session and returns `{enabled, jobs}` where each job contains `{id, photo_id, station_id, date, filename, state, response_id, model, passes, result, usage, error, created_at, updated_at}`.
- When `RICE_ANALYSIS_ENABLED=true` is set on the server with `RICE_OPENAI_API_KEY`, new photo uploads (`PUT /api/device/photos/...`) automatically queue one analysis job in SQLite (`analysis_jobs`) and immediately respond to the ESP32 camera station so it can sleep without waiting.
- Analysis executes asynchronously in a dedicated worker thread using the OpenAI Responses API (`/v1/responses`) with schema-enforced structured outputs (tiller count, verification checks within evaluation, growth stage, water level in cm when readable, confidence scores, and notes).
- Duplicate photo uploads return `{ok: true, duplicate: true}` without queueing duplicate analysis jobs. Historical data imports do not queue jobs or trigger automatic paid backfill.
- The dashboard displays queued, processing (`submitting` / `submitted`), completed, failed, and uncertain jobs in the Analysis history tab, linking completed reports directly to photographs and distinguishing them from imported archives.
