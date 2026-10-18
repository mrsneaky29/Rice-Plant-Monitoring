// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "cloud_run_mode.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <WiFi.h>
#include "board_config.h"
#include "sd_storage.h"
#include "photo_capture.h"
#include "capture_timer.h"
#include "time_keeper.h"
#include "settings_store.h"
#include "device_log.h"
#include "dht_sensor.h"
#include "sensor_timer.h"
#include "sensor_storage.h"
#include "water_level.h"
#include "water_level_storage.h"
#include "web_page.h"
#include "ads1115.h"
#include "station_identity.h"
#include "cloud_device.h"

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#endif

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

static esp_err_t send_json(httpd_req_t *req, const char *json) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json, strlen(json));
}

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, PAGE_INDEX_HTML, strlen(PAGE_INDEX_HTML));
}

static esp_err_t status_handler(httpd_req_t *req) {
  char json[1800];
  char timeStr[24] = "";
  char stationId[16], stationName[32], hostname[24];
  stationIdentityId(stationId, sizeof(stationId));
  stationIdentityHostname(hostname, sizeof(hostname));
  settingsStationName(stationName, sizeof(stationName));
  timeKeeperLocalDateTime(timeStr, sizeof(timeStr));

  float dhtTemp = 0, dhtHum = 0;
  uint32_t dhtAgeS = 0;
  bool haveDht = dhtSensorLastReading(&dhtTemp, &dhtHum, &dhtAgeS);

  float adsV = 0;
  bool adsOk = ads1115Present() && ads1115ReadA0(&adsV);

  snprintf(
    json, sizeof(json),
    "{\"station_id\":\"%s\",\"station_no\":%d,\"station_name\":\"%s\",\"sensor_role\":\"%s\",\"mac\":\"%s\","
    "\"sd_mounted\":%s,\"photo_count\":%u,\"free_mb\":%llu,\"total_mb\":%llu,"
    "\"timer_enabled\":%s,\"timer_interval\":%u,\"sd_full\":%s,\"streaming\":%s,"
    "\"ip\":\"%s\",\"rssi\":%d,\"ssid\":\"%s\",\"mdns\":\"%s.local\","
    "\"time_set\":%s,\"time\":\"%s\",\"uptime_s\":%lu,\"erasing\":%s,\"flash_enabled\":%s,\"torch_on\":%s,"
    "\"win\":%s,\"wstart\":%u,\"wend\":%u,\"in_window\":%s,\"camera_ok\":%s,"
    "\"dht_enabled\":%s,\"dht_interval\":%u,\"dht_win\":%s,\"dht_wstart\":%u,\"dht_wend\":%u,\"dht_in_window\":%s,"
    "\"dht_have\":%s,"
    "\"cloud_enabled\":%s,\"sleep_enabled\":%s,\"awake\":%s,\"stay_awake\":%s,\"maintenance_remaining_seconds\":%u,\"upload_error_count\":%u,\"last_upload_error\":\"%s\","
    "\"ai_vision_enabled\":%s,\"openai_key_set\":%s,"
    "\"ads_ok\":%s,\"ads_addr\":%u",
    stationId, settingsStationNumber(), stationName, settingsStationRoleName(settingsStationRole()), WiFi.macAddress().c_str(),
    sdStorageIsMounted() ? "true" : "false", (unsigned)sdStoragePhotoCount(),
    (unsigned long long)(sdStorageFreeBytes() / (1024 * 1024)),
    (unsigned long long)(sdStorageTotalBytes() / (1024 * 1024)),
    captureTimerIsEnabled() ? "true" : "false", (unsigned)captureTimerIntervalSeconds(),
    sdStorageIsFull() ? "true" : "false", photoCaptureIsStreaming() ? "true" : "false",
    WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), WiFi.SSID().c_str(), hostname,
    timeKeeperIsSet() ? "true" : "false", timeStr,
    (unsigned long)(esp_timer_get_time() / 1000000ULL),
    sdStorageIsErasing() ? "true" : "false",
    photoCaptureFlashEnabled() ? "true" : "false",
    photoCaptureTorchOn() ? "true" : "false",
    captureTimerWindowEnabled() ? "true" : "false", (unsigned)captureTimerWindowStart(), (unsigned)captureTimerWindowEnd(),
    captureTimerInWindow() ? "true" : "false",
    photoCaptureCameraOk() ? "true" : "false",
    sensorTimerIsEnabled() ? "true" : "false", (unsigned)sensorTimerIntervalSeconds(),
    sensorTimerWindowEnabled() ? "true" : "false", (unsigned)sensorTimerWindowStart(), (unsigned)sensorTimerWindowEnd(),
    sensorTimerInWindow() ? "true" : "false",
    haveDht ? "true" : "false",
    cloudDeviceConfigured() ? "true" : "false", cloudDeviceSleepEnabled() ? "true" : "false",
    WiFi.status() == WL_CONNECTED ? "true" : "false", cloudDeviceStayAwake() ? "true" : "false",
    (unsigned)cloudDeviceMaintenanceRemainingSeconds(),
    (unsigned)cloudDeviceUploadErrorCount(), cloudDeviceLastUploadError(),
    waterLevelVisionEnabled() ? "true" : "false",
    settingsOpenAiKeySet() ? "true" : "false",
    adsOk ? "true" : "false", (unsigned)ads1115Address()
  );
  size_t used = strlen(json);
  if (haveDht && used < sizeof(json)) {
    used += snprintf(json + used, sizeof(json) - used, ",\"dht_c\":%.1f,\"dht_h\":%.1f,\"dht_age_s\":%lu",
                     dhtTemp, dhtHum, (unsigned long)dhtAgeS);
  }
  if (adsOk && used < sizeof(json)) {
    used += snprintf(json + used, sizeof(json) - used, ",\"ads_a0\":%.4f,\"ads_m0\":%.1f",
                     adsV, ads1115MoisturePercent(adsV));
  }
  if (used < sizeof(json) - 1) strcat(json, "}");
  return send_json(req, json);
}

static esp_err_t snap_handler(httpd_req_t *req) {
  char filename[48] = { 0 };
  bool ok = photoCaptureSnap(filename, sizeof(filename));

  char json[160];
  if (ok) {
    deviceLog("[capture] manual snap saved %s", filename);
    snprintf(json, sizeof(json), "{\"ok\":true,\"filename\":\"%s\",\"count\":%u}", filename, (unsigned)sdStoragePhotoCount());
  } else {
    const char *reason = sdStorageIsErasing() ? "erasing" : (sdStorageIsFull() ? "sd_full" : "capture_failed");
    deviceLog("[capture] manual snap failed: %s", reason);
    snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", reason);
  }
  return send_json(req, json);
}

static esp_err_t latest_handler(httpd_req_t *req) {
  const uint8_t *buf = NULL;
  size_t len = 0;
  if (!photoCaptureLockLatest(&buf, &len)) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char *)buf, len);
  photoCaptureUnlockLatest();
  return res;
}

// Copies the query value for `key` into val (empty string when missing).
static bool get_query_str(httpd_req_t *req, const char *key, char *val, size_t valSize) {
  val[0] = '\0';
  size_t qlen = httpd_req_get_url_query_len(req) + 1;
  if (qlen <= 1) {
    return false;
  }
  char *buf = (char *)malloc(qlen);
  if (!buf) {
    return false;
  }
  bool found = false;
  if (httpd_req_get_url_query_str(req, buf, qlen) == ESP_OK) {
    found = httpd_query_key_value(buf, key, val, valSize) == ESP_OK;
  }
  free(buf);
  return found;
}

static int get_query_int(httpd_req_t *req, const char *key, int def) {
  char val[16];
  if (!get_query_str(req, key, val, sizeof(val))) {
    return def;
  }
  return atoi(val);
}

// GET /device/config?station=1..3&role=soil|dht22|none&name=Plant%201
// A reboot is required because mDNS/OTA hostnames and hardware pins are
// initialized only once during startup.
static esp_err_t device_config_handler(httpd_req_t *req) {
  int station = get_query_int(req, "station", settingsStationNumber());
  char roleText[12] = "";
  char name[32] = "";
  get_query_str(req, "role", roleText, sizeof(roleText));
  get_query_str(req, "name", name, sizeof(name));
  StationRole role = settingsStationRole();
  if (roleText[0]) {
    if (strcmp(roleText, "soil") == 0) role = STATION_ROLE_SOIL;
    else if (strcmp(roleText, "dht22") == 0) role = STATION_ROLE_DHT22;
    else if (strcmp(roleText, "none") == 0) role = STATION_ROLE_NONE;
    else return send_json(req, "{\"ok\":false,\"error\":\"invalid_role\"}");
  }
  if (!settingsSetStationConfig(station, role, name[0] ? name : NULL)) {
    return send_json(req, "{\"ok\":false,\"error\":\"invalid_config\"}");
  }
  char json[160];
  snprintf(json, sizeof(json), "{\"ok\":true,\"station_no\":%d,\"role\":\"%s\",\"reboot_required\":true}", station, settingsStationRoleName(role));
  return send_json(req, json);
}

// GET /timer?enable=0/1&interval=SEC&win=0/1&wstart=MIN&wend=MIN
// win/wstart/wend are optional; when omitted the existing window setting
// is left untouched. wstart/wend are minutes since local midnight.
static esp_err_t timer_handler(httpd_req_t *req) {
  int enable = get_query_int(req, "enable", -1);
  int interval = get_query_int(req, "interval", -1);
  int win = get_query_int(req, "win", -1);
  int wstart = get_query_int(req, "wstart", -1);
  int wend = get_query_int(req, "wend", -1);

  if (interval > 0) {
    captureTimerSetIntervalSeconds((uint32_t)interval);
  }
  if (win >= 0 || wstart >= 0 || wend >= 0) {
    bool newWin = win >= 0 ? (win == 1) : captureTimerWindowEnabled();
    uint16_t newStart = wstart >= 0 ? (uint16_t)wstart : captureTimerWindowStart();
    uint16_t newEnd = wend >= 0 ? (uint16_t)wend : captureTimerWindowEnd();
    captureTimerSetWindow(newWin, newStart, newEnd);
  }
  if (enable == 0 || enable == 1) {
    captureTimerSetEnabled(enable == 1);
  }
  settingsSaveTimer(captureTimerIsEnabled(), captureTimerIntervalSeconds(), captureTimerWindowEnabled(), captureTimerWindowStart(), captureTimerWindowEnd());
  deviceLog("[timer] capture schedule updated: enabled=%s interval=%us window=%s(%u-%u)",
            captureTimerIsEnabled() ? "yes" : "no", (unsigned)captureTimerIntervalSeconds(),
            captureTimerWindowEnabled() ? "on" : "off", (unsigned)captureTimerWindowStart(), (unsigned)captureTimerWindowEnd());

  char json[192];
  snprintf(
    json, sizeof(json),
    "{\"enabled\":%s,\"interval\":%u,\"win\":%s,\"wstart\":%u,\"wend\":%u,\"in_window\":%s}",
    captureTimerIsEnabled() ? "true" : "false", (unsigned)captureTimerIntervalSeconds(),
    captureTimerWindowEnabled() ? "true" : "false", (unsigned)captureTimerWindowStart(), (unsigned)captureTimerWindowEnd(),
    captureTimerInWindow() ? "true" : "false"
  );
  return send_json(req, json);
}

// GET /sleep?enable=0/1&awake=0/1&resume=1
static esp_err_t sleep_handler(httpd_req_t *req) {
  int enable = get_query_int(req, "enable", -1);
  int awake = get_query_int(req, "awake", -1);
  int resume = get_query_int(req, "resume", 0);
  if (enable == 1 && !cloudDeviceConfigured()) {
    return send_json(req, "{\"ok\":false,\"error\":\"cloud_not_configured\"}");
  }
  if (enable == 0 || enable == 1) cloudDeviceSetSleepEnabled(enable == 1);
  if (awake == 0 || awake == 1) cloudDeviceRequestStayAwake(awake == 1);
  if (resume == 1) cloudDeviceRequestStayAwake(false);
  char json[240];
  snprintf(json, sizeof(json),
    "{\"ok\":true,\"cloud_enabled\":%s,\"sleep_enabled\":%s,\"awake\":%s,\"maintenance_remaining_seconds\":%u}",
    cloudDeviceConfigured() ? "true" : "false", cloudDeviceSleepEnabled() ? "true" : "false",
    cloudDeviceStayAwake() ? "true" : "false", (unsigned)cloudDeviceMaintenanceRemainingSeconds());
  return send_json(req, json);
}

static esp_err_t flash_handler(httpd_req_t *req) {
  int enable = get_query_int(req, "enable", -1);
  if (enable == 0 || enable == 1) {
    photoCaptureSetFlashEnabled(enable == 1);
    settingsSetFlashEnabled(enable == 1);
    deviceLog("[capture] flash-on-capture set to %s", enable == 1 ? "on" : "off");
  }
  char json[64];
  snprintf(json, sizeof(json), "{\"flash_enabled\":%s}", photoCaptureFlashEnabled() ? "true" : "false");
  return send_json(req, json);
}

// GET /torch?enable=0/1 — steady live-view light, independent of the
// capture flash. Not persisted; always off after a reboot.
static esp_err_t torch_handler(httpd_req_t *req) {
  int enable = get_query_int(req, "enable", -1);
  if (enable == 0 || enable == 1) {
    photoCaptureSetTorch(enable == 1);
  }
  char json[64];
  snprintf(json, sizeof(json), "{\"torch_on\":%s}", photoCaptureTorchOn() ? "true" : "false");
  return send_json(req, json);
}

static esp_err_t time_handler(httpd_req_t *req) {
  char val[16];
  if (get_query_str(req, "epoch", val, sizeof(val))) {
    uint32_t epoch = (uint32_t)strtoul(val, NULL, 10);
    int tz = get_query_int(req, "tz", timeKeeperTzOffsetMinutes());
    timeKeeperSetFromEpoch(epoch, tz);
  }

  char timeStr[24] = "";
  timeKeeperLocalDateTime(timeStr, sizeof(timeStr));
  char json[96];
  snprintf(json, sizeof(json), "{\"time_set\":%s,\"time\":\"%s\",\"tz\":%d}", timeKeeperIsSet() ? "true" : "false", timeStr, timeKeeperTzOffsetMinutes());
  return send_json(req, json);
}

static esp_err_t dates_handler(httpd_req_t *req) {
  char *json = (char *)malloc(4096);
  if (!json) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  if (!sdStorageDatesJson(json, 4096)) {
    snprintf(json, 4096, "{\"dates\":[]}");
  }
  esp_err_t res = send_json(req, json);
  free(json);
  return res;
}

static esp_err_t photos_handler(httpd_req_t *req) {
  char date[16];
  if (!get_query_str(req, "date", date, sizeof(date))) {
    return send_json(req, "{\"error\":\"missing date\"}");
  }
  int page = get_query_int(req, "page", 0);
  int per = get_query_int(req, "per", 10);
  if (per < 1) per = 10;
  if (per > 500) per = 500;
  char fromTime[8] = { 0 };
  char toTime[8] = { 0 };
  get_query_str(req, "from", fromTime, sizeof(fromTime));
  get_query_str(req, "to", toTime, sizeof(toTime));

  // Buffer sized to the actual matching count (per-page window is at most
  // that many entries) so "unlimited" requests don't get silently truncated.
  int matching = sdStoragePhotoCountForDate(date, fromTime, toTime);
  int entries = per < matching ? per : matching;
  size_t bufSize = 256 + (size_t)(entries + 4) * 48;
  if (bufSize < 512) bufSize = 512;
  if (bufSize > 65536) bufSize = 65536;

  char *json = (char *)malloc(bufSize);
  if (!json) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  if (!sdStoragePhotosJson(date, page, per, fromTime, toTime, json, bufSize)) {
    snprintf(json, bufSize, "{\"error\":\"bad request\"}");
  }
  esp_err_t res = send_json(req, json);
  free(json);
  return res;
}

/* ---------- DHT22 temperature/humidity sensor ---------- */

// GET /dht/now — takes an immediate reading (blocking, ~a few ms) and
// saves it, independent of the schedule.
static esp_err_t dht_now_handler(httpd_req_t *req) {
  float t, h;
  char json[128];
  if (dhtSensorRead(&t, &h)) {
    bool saved = sensorStorageAppend(t, h);
    deviceLog("[dht] manual reading: %.1fC %.1f%% RH%s", t, h, saved ? "" : " (failed to save)");
    snprintf(json, sizeof(json), "{\"ok\":true,\"c\":%.1f,\"h\":%.1f,\"saved\":%s}", t, h, saved ? "true" : "false");
  } else {
    deviceLog("[dht] manual reading failed (sensor timeout/checksum)");
    snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"read_failed\"}");
  }
  return send_json(req, json);
}

// GET /dht/config?enable=0/1&interval=SEC&win=0/1&wstart=MIN&wend=MIN
// Same shape as /timer, for the DHT22 schedule.
static esp_err_t dht_config_handler(httpd_req_t *req) {
  int enable = get_query_int(req, "enable", -1);
  int interval = get_query_int(req, "interval", -1);
  int win = get_query_int(req, "win", -1);
  int wstart = get_query_int(req, "wstart", -1);
  int wend = get_query_int(req, "wend", -1);

  if (interval > 0) {
    sensorTimerSetIntervalSeconds((uint32_t)interval);
  }
  if (win >= 0 || wstart >= 0 || wend >= 0) {
    bool newWin = win >= 0 ? (win == 1) : sensorTimerWindowEnabled();
    uint16_t newStart = wstart >= 0 ? (uint16_t)wstart : sensorTimerWindowStart();
    uint16_t newEnd = wend >= 0 ? (uint16_t)wend : sensorTimerWindowEnd();
    sensorTimerSetWindow(newWin, newStart, newEnd);
  }
  if (enable == 0 || enable == 1) {
    sensorTimerSetEnabled(enable == 1);
  }
  settingsSaveDhtTimer(sensorTimerIsEnabled(), sensorTimerIntervalSeconds(), sensorTimerWindowEnabled(), sensorTimerWindowStart(), sensorTimerWindowEnd());
  deviceLog("[dht] reading schedule updated: enabled=%s interval=%us window=%s(%u-%u)",
            sensorTimerIsEnabled() ? "yes" : "no", (unsigned)sensorTimerIntervalSeconds(),
            sensorTimerWindowEnabled() ? "on" : "off", (unsigned)sensorTimerWindowStart(), (unsigned)sensorTimerWindowEnd());

  char json[160];
  snprintf(
    json, sizeof(json), "{\"enabled\":%s,\"interval\":%u,\"win\":%s,\"wstart\":%u,\"wend\":%u,\"in_window\":%s}",
    sensorTimerIsEnabled() ? "true" : "false", (unsigned)sensorTimerIntervalSeconds(),
    sensorTimerWindowEnabled() ? "true" : "false", (unsigned)sensorTimerWindowStart(), (unsigned)sensorTimerWindowEnd(),
    sensorTimerInWindow() ? "true" : "false"
  );
  return send_json(req, json);
}

// GET /dht/list?date=YYYY-MM-DD&page=&per=&from=&to= — same paging/time-
// filter scheme as /photos, applied to that date's sensors.csv.
static esp_err_t dht_list_handler(httpd_req_t *req) {
  char date[16];
  if (!get_query_str(req, "date", date, sizeof(date))) {
    return send_json(req, "{\"error\":\"missing date\"}");
  }
  int page = get_query_int(req, "page", 0);
  int per = get_query_int(req, "per", 20);
  if (per < 1) per = 20;
  if (per > 100) per = 100;
  char fromTime[8] = { 0 };
  char toTime[8] = { 0 };
  get_query_str(req, "from", fromTime, sizeof(fromTime));
  get_query_str(req, "to", toTime, sizeof(toTime));

  size_t bufSize = 256 + (size_t)(per + 4) * 40;
  if (bufSize > 100000) bufSize = 100000;

  char *json = (char *)malloc(bufSize);
  if (!json) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  if (!sensorStorageReadingsJson(date, page, per, fromTime, toTime, json, bufSize)) {
    snprintf(json, bufSize, "{\"error\":\"bad request\"}");
  }
  esp_err_t res = send_json(req, json);
  free(json);
  return res;
}

// GET /dht/eraseall?confirm=ERASE — deletes all DHT reading history
// (sensors.csv from every date folder), leaves photos untouched.
static esp_err_t dht_eraseall_handler(httpd_req_t *req) {
  char confirm[8];
  get_query_str(req, "confirm", confirm, sizeof(confirm));
  if (strcmp(confirm, "ERASE") != 0) {
    return send_json(req, "{\"ok\":false,\"error\":\"confirm_required\"}");
  }
  int removed = sensorStorageEraseAll();
  deviceLog("[dht] erased all reading history (%d file(s))", removed);
  char json[64];
  snprintf(json, sizeof(json), "{\"ok\":true,\"removed\":%d}", removed);
  return send_json(req, json);
}

// GET /log?lines=100 — tail of the device log (plain text, newest last).
static esp_err_t log_handler(httpd_req_t *req) {
  int lines = get_query_int(req, "lines", 100);
  size_t bufSize = 16384;
  char *buf = (char *)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) {
    bufSize = 4096;
    buf = (char *)malloc(bufSize);
  }
  if (!buf) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  bool ok = deviceLogTail(lines, buf, bufSize);
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, ok ? buf : "(no log yet)", HTTPD_RESP_USE_STRLEN);
  free(buf);
  return res;
}

/* ---------- Water level (OpenAI vision) ---------- */

// GET /water/vision?enable=0/1 — turns AI vision analysis on/off. When on,
// every newly captured photo (any path: manual, scheduled, live-view
// snap) is queued for background analysis automatically; there's no
// separate manual trigger or schedule of its own.
static esp_err_t water_vision_handler(httpd_req_t *req) {
  int enable = get_query_int(req, "enable", -1);
  if (enable == 1 && !settingsOpenAiKeySet()) {
    return send_json(req, "{\"ai_vision_enabled\":false,\"error\":\"key_required\"}");
  }
  if (enable == 0 || enable == 1) {
    waterLevelSetVisionEnabled(enable == 1);
    settingsSetAiVisionEnabled(enable == 1);
    deviceLog("[water] AI vision analysis set to %s", enable == 1 ? "on" : "off");
  }
  char json[64];
  snprintf(json, sizeof(json), "{\"ai_vision_enabled\":%s}", waterLevelVisionEnabled() ? "true" : "false");
  return send_json(req, json);
}

// POST /water/analyze?date=YYYY-MM-DD&name=PHOTO.jpg — queues one existing
// gallery photo for analysis. This is independent of the automatic-analysis
// switch: clicking the gallery action is explicit permission for this photo.
static esp_err_t water_analyze_handler(httpd_req_t *req) {
  char date[16], name[40];
  if (!get_query_str(req, "date", date, sizeof(date)) ||
      !get_query_str(req, "name", name, sizeof(name))) {
    return send_json(req, "{\"ok\":false,\"error\":\"missing_photo\"}");
  }
  if (!settingsOpenAiKeySet()) {
    return send_json(req, "{\"ok\":false,\"error\":\"key_required\"}");
  }

  bool photoExists = false;
  if (sdStorageLock(3000)) {
    File photo = sdStorageOpenPhoto(date, name);
    photoExists = (bool)photo;
    if (photo) photo.close();
    sdStorageUnlock();
  }
  if (!photoExists) {
    return send_json(req, "{\"ok\":false,\"error\":\"photo_not_found\"}");
  }

  WaterLevelQueueResult result = waterLevelEnqueueManualAnalysis(date, name);
  switch (result) {
    case WATER_LEVEL_QUEUE_QUEUED:
      return send_json(req, "{\"ok\":true,\"queued\":true}");
    case WATER_LEVEL_QUEUE_ALREADY_PENDING:
      return send_json(req, "{\"ok\":true,\"queued\":false,\"already_pending\":true}");
    case WATER_LEVEL_QUEUE_FULL:
      return send_json(req, "{\"ok\":false,\"error\":\"queue_full\"}");
    case WATER_LEVEL_QUEUE_UNAVAILABLE:
      return send_json(req, "{\"ok\":false,\"error\":\"analysis_unavailable\"}");
    default:
      return send_json(req, "{\"ok\":false,\"error\":\"invalid_photo\"}");
  }
}

// GET /water/list?date=YYYY-MM-DD&page=&per=&from=&to=
static esp_err_t water_list_handler(httpd_req_t *req) {
  char date[16];
  if (!get_query_str(req, "date", date, sizeof(date))) {
    return send_json(req, "{\"error\":\"missing date\"}");
  }
  int page = get_query_int(req, "page", 0);
  int per = get_query_int(req, "per", 20);
  if (per < 1) per = 20;
  if (per > 2000) per = 2000;
  char fromTime[8] = { 0 };
  char toTime[8] = { 0 };
  get_query_str(req, "from", fromTime, sizeof(fromTime));
  get_query_str(req, "to", toTime, sizeof(toTime));

  // A plant-analysis row includes a growth stage and notes, so reserve
  // enough response space for the full structured result.
  size_t bufSize = 512 + (size_t)(per + 4) * 700;
  if (bufSize > 74000) bufSize = 74000;

  char *json = (char *)malloc(bufSize);
  if (!json) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  if (!waterLevelStorageReadingsJson(date, page, per, fromTime, toTime, json, bufSize)) {
    snprintf(json, bufSize, "{\"error\":\"bad request\"}");
  }
  esp_err_t res = send_json(req, json);
  free(json);
  return res;
}

// POST /water/apikey with a text/plain body. Keeping the secret out of the
// URL prevents it from being copied into browser history and access logs.
static esp_err_t water_apikey_handler(httpd_req_t *req) {
  char key[OPENAI_API_KEY_MAX];
  if (req->content_len <= 0 || req->content_len >= (int)sizeof(key)) {
    return send_json(req, "{\"ok\":false,\"error\":\"key_required\"}");
  }
  size_t received = 0;
  while (received < (size_t)req->content_len) {
    int n = httpd_req_recv(req, key + received, req->content_len - received);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) return send_json(req, "{\"ok\":false,\"error\":\"receive_failed\"}");
    received += (size_t)n;
  }
  key[received] = '\0';
  while (received > 0 && (key[received - 1] == '\r' || key[received - 1] == '\n' || key[received - 1] == ' ')) {
    key[--received] = '\0';
  }
  if (!key[0]) return send_json(req, "{\"ok\":false,\"error\":\"key_required\"}");
  settingsSetOpenAiKey(key);
  deviceLog("[analysis] OpenAI API key updated (%u chars)", (unsigned)strlen(key));
  return send_json(req, "{\"ok\":true}");
}

// GET /water/eraseall?confirm=ERASE — deletes all plant-analysis history
// (plant_analysis.csv and legacy water_level.csv from every date folder). The source photos
// each reading references stay in the gallery.
static esp_err_t water_eraseall_handler(httpd_req_t *req) {
  char confirm[8];
  get_query_str(req, "confirm", confirm, sizeof(confirm));
  if (strcmp(confirm, "ERASE") != 0) {
    return send_json(req, "{\"ok\":false,\"error\":\"confirm_required\"}");
  }
  int removed = waterLevelStorageEraseAll();
  deviceLog("[analysis] erased all analysis history (%d file(s))", removed);
  char json[64];
  snprintf(json, sizeof(json), "{\"ok\":true,\"removed\":%d}", removed);
  return send_json(req, json);
}

// Streams one photo from SD in 8KB chunks so large files never hang the
// server or exhaust RAM. ?dl=1 forces a browser download with a
// date-stamped filename.
static esp_err_t photo_handler(httpd_req_t *req) {
  char date[16], name[48];
  bool haveDate = get_query_str(req, "date", date, sizeof(date));
  bool haveName = get_query_str(req, "name", name, sizeof(name));
  if (!haveDate || !haveName) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  if (!sdStorageLock(3000)) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  File f = sdStorageOpenPhoto(date, name);
  if (!f) {
    sdStorageUnlock();
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  char disp[80];
  if (get_query_int(req, "dl", 0) == 1) {
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s_%s\"", date, name);
  } else {
    snprintf(disp, sizeof(disp), "inline; filename=\"%s_%s\"", date, name);
  }
  httpd_resp_set_hdr(req, "Content-Disposition", disp);

  const size_t chunkSize = 8192;
  uint8_t *chunk = (uint8_t *)malloc(chunkSize);
  if (!chunk) {
    f.close();
    sdStorageUnlock();
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  esp_err_t res = ESP_OK;
  while (res == ESP_OK) {
    size_t n = f.read(chunk, chunkSize);
    if (n == 0) {
      break;
    }
    res = httpd_resp_send_chunk(req, (const char *)chunk, n);
  }
  f.close();
  sdStorageUnlock();
  free(chunk);

  if (res == ESP_OK) {
    res = httpd_resp_send_chunk(req, NULL, 0);
  } else {
    httpd_resp_sendstr_chunk(req, NULL);
  }
  return res;
}

// Kept low enough that date + up to this many ~30-char filenames stay
// under the esp_http_server default max URI/query length (~512 bytes).
// The web page chunks larger "select all" batches into calls this size.
#define MAX_BATCH_FILES 15

// Splits a comma-separated "files" query value into names. Returns count.
static int split_files(char *list, char *names[], int maxNames) {
  int count = 0;
  char *save = NULL;
  char *tok = strtok_r(list, ",", &save);
  while (tok && count < maxNames) {
    if (tok[0]) {
      names[count++] = tok;
    }
    tok = strtok_r(NULL, ",", &save);
  }
  return count;
}

// GET /delete?date=YYYY-MM-DD&files=a.jpg,b.jpg  (max 20 per call)
static esp_err_t delete_handler(httpd_req_t *req) {
#if RICE_SD_PROTECT_PHOTOS
  httpd_resp_set_status(req, "403 Forbidden");
  return send_json(req, "{\"ok\":false,\"error\":\"sd_photos_protected\"}");
#endif
  char date[16];
  char list[640];
  if (!get_query_str(req, "date", date, sizeof(date)) || !get_query_str(req, "files", list, sizeof(list))) {
    return send_json(req, "{\"ok\":false,\"error\":\"missing params\"}");
  }

  char *names[MAX_BATCH_FILES];
  int count = split_files(list, names, MAX_BATCH_FILES);
  int deleted = 0;
  for (int i = 0; i < count; i++) {
    if (sdStorageDeletePhoto(date, names[i])) {
      deleted++;
    }
  }

  deviceLog("[gallery] deleted %d/%d photo(s) from %s", deleted, count, date);

  char json[96];
  snprintf(json, sizeof(json), "{\"ok\":true,\"deleted\":%d,\"requested\":%d}", deleted, count);
  return send_json(req, json);
}

// GET /format?confirm=ERASE — wipes all photos in a background task.
static esp_err_t format_handler(httpd_req_t *req) {
#if RICE_SD_PROTECT_PHOTOS
  httpd_resp_set_status(req, "403 Forbidden");
  return send_json(req, "{\"ok\":false,\"error\":\"sd_photos_protected\"}");
#endif
  char confirm[8];
  get_query_str(req, "confirm", confirm, sizeof(confirm));
  if (strcmp(confirm, "ERASE") != 0) {
    return send_json(req, "{\"ok\":false,\"error\":\"confirm_required\"}");
  }
  captureTimerSetEnabled(false);
  settingsSaveTimer(false, captureTimerIntervalSeconds(), captureTimerWindowEnabled(), captureTimerWindowStart(), captureTimerWindowEnd());
  bool started = sdStorageEraseAllStart();
  return send_json(req, started ? "{\"ok\":true,\"erasing\":true}" : "{\"ok\":false,\"error\":\"not_started\"}");
}

/* ---------- WiFi network management (saved in NVS, not on the SD card) ---------- */

static esp_err_t wifi_list_handler(httpd_req_t *req) {
  char json[512];
  size_t pos = 0;
  pos += snprintf(json + pos, sizeof(json) - pos, "{\"connected\":\"%s\",\"networks\":[", WiFi.SSID().c_str());
  int count = settingsWifiCount();
  for (int i = 0; i < count; i++) {
    WifiNetwork n;
    if (!settingsWifiGet(i, &n)) {
      continue;
    }
    int written = snprintf(json + pos, sizeof(json) - pos, "%s{\"ssid\":\"%s\"}", i ? "," : "", n.ssid);
    if (written < 0 || (size_t)written >= sizeof(json) - pos) {
      break;
    }
    pos += written;
  }
  pos += snprintf(json + pos, sizeof(json) - pos, "]}");
  return send_json(req, json);
}

// GET /wifi/add?ssid=X&pass=Y — saves/updates a network. Doesn't reconnect
// immediately (the response has to make it back over the current link);
// it's picked up on the next reconnect or reboot.
static esp_err_t wifi_add_handler(httpd_req_t *req) {
  char ssid[WIFI_SSID_MAX], pass[WIFI_PASS_MAX];
  if (!get_query_str(req, "ssid", ssid, sizeof(ssid)) || !ssid[0]) {
    return send_json(req, "{\"ok\":false,\"error\":\"ssid_required\"}");
  }
  get_query_str(req, "pass", pass, sizeof(pass));
  bool ok = settingsWifiAdd(ssid, pass);
  // Never log the password -- SSID only.
  deviceLog("[wifi] network \"%s\" %s", ssid, ok ? "saved" : "failed to save");
  return send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"save_failed\"}");
}

static esp_err_t wifi_delete_handler(httpd_req_t *req) {
  char ssid[WIFI_SSID_MAX];
  if (!get_query_str(req, "ssid", ssid, sizeof(ssid)) || !ssid[0]) {
    return send_json(req, "{\"ok\":false,\"error\":\"ssid_required\"}");
  }
  bool ok = settingsWifiDelete(ssid);
  if (ok) {
    deviceLog("[wifi] network \"%s\" deleted", ssid);
  }
  return send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"not_found\"}");
}

// GET /reboot?confirm=YES — restarts the device shortly after responding
// so the confirmation reaches the browser first.
static esp_err_t reboot_handler(httpd_req_t *req) {
  char confirm[8];
  get_query_str(req, "confirm", confirm, sizeof(confirm));
  if (strcmp(confirm, "YES") != 0) {
    return send_json(req, "{\"ok\":false,\"error\":\"confirm_required\"}");
  }
  deviceLog("[system] reboot requested via web UI");
  esp_err_t res = send_json(req, "{\"ok\":true,\"rebooting\":true}");
  vTaskDelay(pdMS_TO_TICKS(400));
  esp_restart();
  return res;  // unreachable
}

/* ---------- Streaming ZIP (stored entries, no compression) ---------- */

typedef struct {
  char name[56];  // name inside the archive
  uint32_t crc;
  uint32_t size;
  uint32_t offset;
  uint16_t dosTime;
  uint16_t dosDate;
} zip_entry_t;

static void put16(uint8_t *p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
}
static void put32(uint8_t *p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}

// Derives FAT timestamps from "YYYY-MM-DD" + the photo filename so files
// inside the zip keep their capture time. Handles both the current
// "YYYY-MM-DD__HH-MM-SS-mmm.jpg" scheme and legacy "IMG_HHMMSS.jpg" names.
static void zip_entry_times(const char *date, const char *name, uint16_t *dosDate, uint16_t *dosTime) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
  *dosDate = (1 << 5) | 1;  // 1980-01-01 fallback
  *dosTime = 0;
  if (sscanf(date, "%4d-%2d-%2d", &y, &mo, &d) == 3 && y >= 1980) {
    *dosDate = (uint16_t)(((y - 1980) << 9) | (mo << 5) | d);
  }
  int got = sscanf(name, "%*4d-%*2d-%*2d__%2d-%2d-%2d-%3d", &h, &mi, &s, &ms);
  if (got != 4) {
    got = sscanf(name, "IMG_%2d%2d%2d", &h, &mi, &s);
  }
  if (got >= 3) {
    *dosTime = (uint16_t)((h << 11) | (mi << 5) | (s / 2));
  }
}

// GET /zip?date=YYYY-MM-DD&files=a.jpg,b.jpg — streams a store-only zip
// of the selected photos. Uses data descriptors (flag bit 3) so each
// file's CRC can be computed while streaming, with one read per file.
static esp_err_t zip_handler(httpd_req_t *req) {
  char date[16];
  char list[640];
  if (!get_query_str(req, "date", date, sizeof(date)) || !get_query_str(req, "files", list, sizeof(list))) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  char *names[MAX_BATCH_FILES];
  int count = split_files(list, names, MAX_BATCH_FILES);
  if (count == 0) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  zip_entry_t *entries = (zip_entry_t *)calloc(count, sizeof(zip_entry_t));
  uint8_t *chunk = (uint8_t *)malloc(8192);
  if (!entries || !chunk) {
    free(entries);
    free(chunk);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/zip");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  char disp[80];
  snprintf(disp, sizeof(disp), "attachment; filename=\"photos_%s.zip\"", date);
  httpd_resp_set_hdr(req, "Content-Disposition", disp);

  esp_err_t res = ESP_OK;
  uint32_t offset = 0;
  int written = 0;
  uint8_t hdr[128];

  for (int i = 0; i < count && res == ESP_OK; i++) {
    if (!sdStorageLock(3000)) {
      res = ESP_FAIL;
      break;
    }
    File f = sdStorageOpenPhoto(date, names[i]);
    if (!f) {
      sdStorageUnlock();
      continue;  // skip missing files, keep the archive valid
    }
    zip_entry_t *e = &entries[written];
    snprintf(e->name, sizeof(e->name), "%s_%s", date, names[i]);
    e->offset = offset;
    e->size = (uint32_t)f.size();
    zip_entry_times(date, names[i], &e->dosDate, &e->dosTime);

    uint16_t nameLen = (uint16_t)strlen(e->name);
    // Local file header (crc/sizes deferred to the data descriptor)
    put32(hdr + 0, 0x04034b50);
    put16(hdr + 4, 20);        // version needed
    put16(hdr + 6, 0x0008);    // flags: data descriptor
    put16(hdr + 8, 0);         // method: stored
    put16(hdr + 10, e->dosTime);
    put16(hdr + 12, e->dosDate);
    put32(hdr + 14, 0);        // crc
    put32(hdr + 18, 0);        // compressed size
    put32(hdr + 22, 0);        // uncompressed size
    put16(hdr + 26, nameLen);
    put16(hdr + 28, 0);        // extra len
    memcpy(hdr + 30, e->name, nameLen);
    res = httpd_resp_send_chunk(req, (const char *)hdr, 30 + nameLen);
    offset += 30 + nameLen;

    uint32_t crc = 0;
    uint32_t total = 0;
    while (res == ESP_OK) {
      size_t n = f.read(chunk, 8192);
      if (n == 0) {
        break;
      }
      crc = esp_rom_crc32_le(crc, chunk, n);
      total += n;
      res = httpd_resp_send_chunk(req, (const char *)chunk, n);
    }
    f.close();
    sdStorageUnlock();
    e->crc = crc;
    e->size = total;
    offset += total;

    if (res == ESP_OK) {
      put32(hdr + 0, 0x08074b50);  // data descriptor
      put32(hdr + 4, crc);
      put32(hdr + 8, total);
      put32(hdr + 12, total);
      res = httpd_resp_send_chunk(req, (const char *)hdr, 16);
      offset += 16;
    }
    written++;
  }

  // Central directory
  uint32_t cdStart = offset;
  for (int i = 0; i < written && res == ESP_OK; i++) {
    zip_entry_t *e = &entries[i];
    uint16_t nameLen = (uint16_t)strlen(e->name);
    put32(hdr + 0, 0x02014b50);
    put16(hdr + 4, 20);  // version made by
    put16(hdr + 6, 20);  // version needed
    put16(hdr + 8, 0x0008);
    put16(hdr + 10, 0);  // stored
    put16(hdr + 12, e->dosTime);
    put16(hdr + 14, e->dosDate);
    put32(hdr + 16, e->crc);
    put32(hdr + 20, e->size);
    put32(hdr + 24, e->size);
    put16(hdr + 28, nameLen);
    put16(hdr + 30, 0);  // extra
    put16(hdr + 32, 0);  // comment
    put16(hdr + 34, 0);  // disk
    put16(hdr + 36, 0);  // internal attrs
    put32(hdr + 38, 0);  // external attrs
    put32(hdr + 42, e->offset);
    memcpy(hdr + 46, e->name, nameLen);
    res = httpd_resp_send_chunk(req, (const char *)hdr, 46 + nameLen);
    offset += 46 + nameLen;
  }

  if (res == ESP_OK) {
    put32(hdr + 0, 0x06054b50);  // end of central directory
    put16(hdr + 4, 0);
    put16(hdr + 6, 0);
    put16(hdr + 8, (uint16_t)written);
    put16(hdr + 10, (uint16_t)written);
    put32(hdr + 12, offset - cdStart);
    put32(hdr + 16, cdStart);
    put16(hdr + 20, 0);
    res = httpd_resp_send_chunk(req, (const char *)hdr, 22);
  }

  free(entries);
  free(chunk);

  if (res == ESP_OK) {
    res = httpd_resp_send_chunk(req, NULL, 0);
  } else {
    httpd_resp_sendstr_chunk(req, NULL);
  }
  return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  struct timeval _timestamp;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t *_jpg_buf = NULL;
  char *part_buf[128];

  if (!photoCaptureTryBeginStream()) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  static int64_t last_frame = 0;
  if (!last_frame) {
    last_frame = esp_timer_get_time();
  }

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    photoCaptureEndStream();
    return res;
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Framerate", "60");

  while (true) {
    // If a full-resolution snap is pending, this switches to UXGA, grabs
    // and saves one frame there, then switches back to SVGA -- live view
    // visibly pauses for under a second rather than the snap being saved
    // at streaming resolution.
    photoCaptureServiceStreamSnap();

    fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
    } else {
      _timestamp.tv_sec = fb->timestamp.tv_sec;
      _timestamp.tv_usec = fb->timestamp.tv_usec;
      if (fb->format != PIXFORMAT_JPEG) {
        bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
        esp_camera_fb_return(fb);
        fb = NULL;
        if (!jpeg_converted) {
          res = ESP_FAIL;
        }
      } else {
        _jpg_buf_len = fb->len;
        _jpg_buf = fb->buf;
      }
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    if (res == ESP_OK) {
      size_t hlen = snprintf((char *)part_buf, 128, _STREAM_PART, _jpg_buf_len, _timestamp.tv_sec, _timestamp.tv_usec);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if (_jpg_buf) {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if (res != ESP_OK) {
      break;
    }
    int64_t fr_end = esp_timer_get_time();
    int64_t frame_time = fr_end - last_frame;
    last_frame = fr_end;
    frame_time /= 1000;

    uint32_t minInterval = photoCaptureStreamMinIntervalMs();
    if (minInterval > 0 && frame_time < (int64_t)minInterval) {
      vTaskDelay(pdMS_TO_TICKS(minInterval - (uint32_t)frame_time));
    }
  }

  photoCaptureEndStream();
  return res;
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 36;
  config.stack_size = 8192;

  httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL };
  httpd_uri_t status_uri = { .uri = "/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL };
  httpd_uri_t device_config_uri = { .uri = "/device/config", .method = HTTP_GET, .handler = device_config_handler, .user_ctx = NULL };
  httpd_uri_t snap_uri = { .uri = "/snap", .method = HTTP_GET, .handler = snap_handler, .user_ctx = NULL };
  httpd_uri_t latest_uri = { .uri = "/latest.jpg", .method = HTTP_GET, .handler = latest_handler, .user_ctx = NULL };
  httpd_uri_t timer_uri = { .uri = "/timer", .method = HTTP_GET, .handler = timer_handler, .user_ctx = NULL };
  httpd_uri_t sleep_uri = { .uri = "/sleep", .method = HTTP_GET, .handler = sleep_handler, .user_ctx = NULL };
  httpd_uri_t flash_uri = { .uri = "/flash", .method = HTTP_GET, .handler = flash_handler, .user_ctx = NULL };
  httpd_uri_t torch_uri = { .uri = "/torch", .method = HTTP_GET, .handler = torch_handler, .user_ctx = NULL };
  httpd_uri_t time_uri = { .uri = "/time", .method = HTTP_GET, .handler = time_handler, .user_ctx = NULL };
  httpd_uri_t dates_uri = { .uri = "/dates", .method = HTTP_GET, .handler = dates_handler, .user_ctx = NULL };
  httpd_uri_t photos_uri = { .uri = "/photos", .method = HTTP_GET, .handler = photos_handler, .user_ctx = NULL };
  httpd_uri_t photo_uri = { .uri = "/photo", .method = HTTP_GET, .handler = photo_handler, .user_ctx = NULL };
  httpd_uri_t delete_uri = { .uri = "/delete", .method = HTTP_GET, .handler = delete_handler, .user_ctx = NULL };
  httpd_uri_t zip_uri = { .uri = "/zip", .method = HTTP_GET, .handler = zip_handler, .user_ctx = NULL };
  httpd_uri_t format_uri = { .uri = "/format", .method = HTTP_GET, .handler = format_handler, .user_ctx = NULL };
  httpd_uri_t wifi_list_uri = { .uri = "/wifi/list", .method = HTTP_GET, .handler = wifi_list_handler, .user_ctx = NULL };
  httpd_uri_t wifi_add_uri = { .uri = "/wifi/add", .method = HTTP_GET, .handler = wifi_add_handler, .user_ctx = NULL };
  httpd_uri_t wifi_delete_uri = { .uri = "/wifi/delete", .method = HTTP_GET, .handler = wifi_delete_handler, .user_ctx = NULL };
  httpd_uri_t reboot_uri = { .uri = "/reboot", .method = HTTP_GET, .handler = reboot_handler, .user_ctx = NULL };
  httpd_uri_t dht_now_uri = { .uri = "/dht/now", .method = HTTP_GET, .handler = dht_now_handler, .user_ctx = NULL };
  httpd_uri_t dht_config_uri = { .uri = "/dht/config", .method = HTTP_GET, .handler = dht_config_handler, .user_ctx = NULL };
  httpd_uri_t dht_list_uri = { .uri = "/dht/list", .method = HTTP_GET, .handler = dht_list_handler, .user_ctx = NULL };
  httpd_uri_t sensors_list_uri = { .uri = "/sensors/list", .method = HTTP_GET, .handler = dht_list_handler, .user_ctx = NULL };
  httpd_uri_t dht_eraseall_uri = { .uri = "/dht/eraseall", .method = HTTP_GET, .handler = dht_eraseall_handler, .user_ctx = NULL };
  httpd_uri_t log_uri = { .uri = "/log", .method = HTTP_GET, .handler = log_handler, .user_ctx = NULL };
  httpd_uri_t water_vision_uri = { .uri = "/water/vision", .method = HTTP_GET, .handler = water_vision_handler, .user_ctx = NULL };
  httpd_uri_t water_analyze_uri = { .uri = "/water/analyze", .method = HTTP_POST, .handler = water_analyze_handler, .user_ctx = NULL };
  httpd_uri_t water_list_uri = { .uri = "/water/list", .method = HTTP_GET, .handler = water_list_handler, .user_ctx = NULL };
  httpd_uri_t water_apikey_uri = { .uri = "/water/apikey", .method = HTTP_POST, .handler = water_apikey_handler, .user_ctx = NULL };
  httpd_uri_t water_eraseall_uri = { .uri = "/water/eraseall", .method = HTTP_GET, .handler = water_eraseall_handler, .user_ctx = NULL };
  httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };

  deviceLog("Web server on port %d", config.server_port);
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &device_config_uri);
    httpd_register_uri_handler(camera_httpd, &snap_uri);
    httpd_register_uri_handler(camera_httpd, &latest_uri);
    httpd_register_uri_handler(camera_httpd, &timer_uri);
    httpd_register_uri_handler(camera_httpd, &sleep_uri);
    httpd_register_uri_handler(camera_httpd, &flash_uri);
    httpd_register_uri_handler(camera_httpd, &torch_uri);
    httpd_register_uri_handler(camera_httpd, &time_uri);
    httpd_register_uri_handler(camera_httpd, &dates_uri);
    httpd_register_uri_handler(camera_httpd, &photos_uri);
    httpd_register_uri_handler(camera_httpd, &photo_uri);
    httpd_register_uri_handler(camera_httpd, &delete_uri);
    httpd_register_uri_handler(camera_httpd, &zip_uri);
    httpd_register_uri_handler(camera_httpd, &format_uri);
    httpd_register_uri_handler(camera_httpd, &wifi_list_uri);
    httpd_register_uri_handler(camera_httpd, &wifi_add_uri);
    httpd_register_uri_handler(camera_httpd, &wifi_delete_uri);
    httpd_register_uri_handler(camera_httpd, &reboot_uri);
    httpd_register_uri_handler(camera_httpd, &dht_now_uri);
    httpd_register_uri_handler(camera_httpd, &dht_config_uri);
    httpd_register_uri_handler(camera_httpd, &dht_list_uri);
    httpd_register_uri_handler(camera_httpd, &sensors_list_uri);
    httpd_register_uri_handler(camera_httpd, &dht_eraseall_uri);
    httpd_register_uri_handler(camera_httpd, &log_uri);
    httpd_register_uri_handler(camera_httpd, &water_vision_uri);
    httpd_register_uri_handler(camera_httpd, &water_analyze_uri);
    httpd_register_uri_handler(camera_httpd, &water_list_uri);
    httpd_register_uri_handler(camera_httpd, &water_apikey_uri);
    httpd_register_uri_handler(camera_httpd, &water_eraseall_uri);
  }

  config.server_port += 1;
  config.ctrl_port += 1;
  deviceLog("Stream server on port %d", config.server_port);
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

void stopCameraServer() {
  if (stream_httpd) {
    httpd_stop(stream_httpd);
    stream_httpd = NULL;
  }
  if (camera_httpd) {
    httpd_stop(camera_httpd);
    camera_httpd = NULL;
  }
}
