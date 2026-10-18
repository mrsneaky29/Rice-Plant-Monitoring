#include "cloud_device.h"
#include "cloud_run_mode.h"
#include "cloud_upload_policy.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp32-hal-ledc.h>
#include <mbedtls/platform.h>
#include <mbedtls/sha256.h>

#include "board_config.h"
#include "capture_timer.h"
#include "device_log.h"
#include "photo_capture.h"
#include "sd_storage.h"
#include "settings_store.h"
#include "station_identity.h"
#include "time_keeper.h"

#if __has_include("cloud_secrets.h")
#include "cloud_secrets.h"
#else
#include "cloud_secrets.example.h"
#endif

#ifndef CLOUD_DEVICE_TOKEN
#define CLOUD_DEVICE_TOKEN ""
#endif
#ifndef CLOUD_ROOT_CA
#define CLOUD_ROOT_CA ""
#endif

static const char *CLOUD_BASE = "https://admin.riceplant.achantalabs.com";
static const uint32_t MAINTENANCE_SECONDS = 600;
static const uint32_t CYCLE_BUDGET_MS = 120000;
static bool configured = false;
static bool stayAwake = false;
static volatile bool wakeCyclePending = false;
static volatile bool scheduledCyclePending = false;
static volatile bool cycleActive = false;
static bool lastSyncOk = false;
static bool otaActive = false;
static SemaphoreHandle_t cycleMutex = NULL;
static uint32_t uploadErrors = 0;
static char lastUploadError[48] = "";
static uint32_t consecutiveSyncFailures = 0;
static uint32_t consecutiveUploadFailures = 0;
static uint32_t nextUploadAllowedMs = 0;
static bool uploadBackoffActive = false;
static bool maintenanceEndedEarly = false;
static uint32_t bootMs = 0;
static uint32_t lastPeriodicSyncMs = 0;

// The bundled SDK otherwise confines TLS allocations to scarce internal RAM.
// Install once, before Wi-Fi/TLS starts, and retain the pair for the whole boot.
static void *cloudTlsCalloc(size_t count, size_t size) {
  if (size && count > SIZE_MAX / size) return NULL;
  void *memory = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!memory) memory = heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return memory;
}

static void cloudTlsFree(void *memory) { heap_caps_free(memory); }

static bool sleepEnabledStored() {
  Preferences p;
  p.begin("cloudcfg", true);
  bool enabled = p.getBool("sleepOn", false);
  p.end();
  return enabled;
}

void cloudDeviceSetSleepEnabled(bool enabled) {
  Preferences p;
  p.begin("cloudcfg", false);
  p.putBool("sleepOn", enabled);
  p.end();
  if (enabled) stayAwake = false;
}

bool cloudDeviceSleepEnabled() { return configured && sleepEnabledStored(); }
bool cloudDeviceConfigured() { return configured; }
bool cloudDeviceStayAwake() { return stayAwake; }
bool cloudDeviceCycleActive() { return cycleActive; }
bool cloudDeviceScheduledPending() { return scheduledCyclePending; }
uint32_t cloudDeviceUploadErrorCount() { return uploadErrors; }
const char *cloudDeviceLastUploadError() { return lastUploadError; }
void cloudDeviceRequestStayAwake(bool enabled) {
  stayAwake = enabled;
  if (!enabled) maintenanceEndedEarly = true;
}
void cloudDeviceSetOtaActive(bool active) { otaActive = active; }

static bool cloudConfiguredFromHeaders() {
  return CLOUD_DEVICE_TOKEN[0] != '\0' && CLOUD_ROOT_CA[0] != '\0';
}

static void configureClient(WiFiClientSecure &client) {
  client.setCACert(CLOUD_ROOT_CA);
  client.setHandshakeTimeout(30);
  client.setTimeout(45000);
}

static bool cloudRequest(const char *method, const String &path, const String &body,
                         String *response, const char *contentType = "application/json",
                         Stream *stream = NULL, size_t streamSize = 0,
                         const char *checksum = NULL) {
#if RICE_CLOUD_STATUS_ONLY
  // Enforce the diagnostic boundary at the network entry point as well as
  // the lifecycle path: no photo body or command result may be transmitted.
  if (strcmp(method, "POST") || path != "/api/device/sync") return false;
#endif
  if (!configured) {
    snprintf(lastUploadError, sizeof(lastUploadError), "not_configured");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    uploadErrors++;
    snprintf(lastUploadError, sizeof(lastUploadError), "wifi_disconnected");
    return false;
  }
  WiFiClientSecure tls;
  configureClient(tls);
  HTTPClient http;
  String url = String(CLOUD_BASE) + path;
  if (!http.begin(tls, url)) {
    uploadErrors++;
    snprintf(lastUploadError, sizeof(lastUploadError), "http_begin_failed");
    deviceLog("[cloud] http.begin failed for %s", path.c_str());
    return false;
  }
  http.setConnectTimeout(15000);
  http.setTimeout(stream ? 45000 : 15000);
  http.addHeader("Authorization", String("Bearer ") + CLOUD_DEVICE_TOKEN);
  if (contentType && contentType[0]) http.addHeader("Content-Type", contentType);
  if (checksum) http.addHeader("X-Content-SHA256", checksum);
  int code;
  if (stream) code = http.sendRequest(method, stream, streamSize);
  else code = http.sendRequest(method, body);
  if (response && code > 0) *response = http.getString();
  bool ok = code >= 200 && code < 300;
  if (!ok) {
    uploadErrors++;
    if (code < 0) {
      char tlsErr[40] = {0};
      tls.lastError(tlsErr, sizeof(tlsErr));
      if (tlsErr[0]) snprintf(lastUploadError, sizeof(lastUploadError), "net_%d:%s", code, tlsErr);
      else snprintf(lastUploadError, sizeof(lastUploadError), "net_%d", code);
      deviceLog("[cloud] %s %s net error: %s", method, path.c_str(), lastUploadError);
      deviceLog("[cloud] internal heap=%u largest=%u PSRAM=%u",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)ESP.getFreePsram());
    } else {
      snprintf(lastUploadError, sizeof(lastUploadError), "http_%d", code);
      char detail[48] = {0};
      if (response && response->length() > 0 && response->indexOf("detail") >= 0) {
        JsonDocument errDoc;
        if (!deserializeJson(errDoc, *response)) {
          const char *d = errDoc["detail"] | "";
          if (d[0]) snprintf(detail, sizeof(detail), " (%s)", d);
        }
      }
      deviceLog("[cloud] %s %s failed (HTTP %d)%s", method, path.c_str(), code, detail);
    }
  }
  http.end();
  return ok;
}

static bool postCommandResult(const String &id, const char *state, const char *message,
                              const char *date = NULL, const char *filename = NULL) {
  JsonDocument doc;
  doc["state"] = state;
  if (message && message[0]) doc["message"] = message;
  if (date && filename) {
    doc["photo"]["date"] = date;
    doc["photo"]["filename"] = filename;
  }
  String body;
  serializeJson(doc, body);
  String ignored;
  return cloudRequest("POST", String("/api/device/commands/") + id + "/result", body, &ignored);
}

static bool persistCommandResult(const String &id, const char *state, const char *message,
                                const char *date = NULL, const char *filename = NULL) {
  JsonDocument result;
  result["state"] = state;
  if (message && message[0]) result["message"] = message;
  if (date && filename) {
    result["photo"]["date"] = date;
    result["photo"]["filename"] = filename;
  }
  String saved;
  serializeJson(result, saved);
  if (!sdStorageSetCloudCommand(id.c_str(), saved.c_str())) return false;
  return postCommandResult(id, state, message, date, filename);
}

static bool payloadMinute(JsonObject payload, const char *key, uint16_t *minute) {
  if (!minute) return false;
  JsonVariant value = payload[key];
  if (value.isNull()) return false;
  if (value.is<uint16_t>()) {
    *minute = value.as<uint16_t>();
    return *minute <= 1439;
  }
  const char *text = value.as<const char *>();
  if (!text) return false;
  int hour = -1, mins = -1;
  if (sscanf(text, "%d:%d", &hour, &mins) != 2 || hour < 0 || hour > 23 || mins < 0 || mins > 59) return false;
  *minute = (uint16_t)(hour * 60 + mins);
  return true;
}

static bool sha256File(File &file, char outHex[65]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  if (mbedtls_sha256_starts(&ctx, 0) != 0) { mbedtls_sha256_free(&ctx); return false; }
  uint8_t buffer[4096], digest[32];
  while (file.available()) {
    size_t n = file.read(buffer, sizeof(buffer));
    if (!n || mbedtls_sha256_update(&ctx, buffer, n) != 0) {
      mbedtls_sha256_free(&ctx);
      return false;
    }
  }
  bool ok = mbedtls_sha256_finish(&ctx, digest) == 0;
  mbedtls_sha256_free(&ctx);
  if (!ok) return false;
  for (size_t i = 0; i < sizeof(digest); i++) snprintf(outHex + i * 2, 3, "%02x", digest[i]);
  outHex[64] = '\0';
  return true;
}

enum UploadResult { UPLOAD_NONE, UPLOAD_DONE, UPLOAD_FAILED };

static UploadResult uploadPhoto(const char *date, const char *filename, uint32_t deadlineMs) {
  String path = String("/api/device/photos/") + date + "/" + filename;
  if (!sdStorageLock(3000)) { uploadErrors++; snprintf(lastUploadError, sizeof(lastUploadError), "sd_busy"); return UPLOAD_FAILED; }
  File file = sdStorageOpenPhoto(date, filename);
  if (!file) { sdStorageUnlock(); uploadErrors++; snprintf(lastUploadError, sizeof(lastUploadError), "open_photo_failed"); return UPLOAD_FAILED; }
  size_t size = file.size();
  if (!size || size > 16U * 1024U * 1024U) { file.close(); sdStorageUnlock(); uploadErrors++; snprintf(lastUploadError, sizeof(lastUploadError), "photo_size_invalid"); return UPLOAD_FAILED; }
  char checksum[65];
  if (!sha256File(file, checksum) || !file.seek(0)) { file.close(); sdStorageUnlock(); uploadErrors++; snprintf(lastUploadError, sizeof(lastUploadError), "checksum_failed"); return UPLOAD_FAILED; }
  if ((int32_t)(millis() - deadlineMs) >= 0) {
    file.close();
    sdStorageUnlock();
    uploadErrors++;
    snprintf(lastUploadError, sizeof(lastUploadError), "upload_deadline_exceeded");
    return UPLOAD_FAILED;
  }
  String ignored;
  bool uploaded = cloudRequest("PUT", path, String(), &ignored, "image/jpeg", &file, size, checksum);
  file.close();
  sdStorageUnlock();
  if (!uploaded) { return UPLOAD_FAILED; }
  if (!sdStorageMarkPhotoUploaded(date, filename)) { uploadErrors++; snprintf(lastUploadError, sizeof(lastUploadError), "upload_ack_write_failed"); return UPLOAD_FAILED; }
  lastUploadError[0] = '\0';
  return UPLOAD_DONE;
}

static UploadResult uploadOnePendingPhoto(uint32_t deadlineMs) {
  char date[16], filename[64];
  if (!sdStorageFindNextUnuploadedPhoto(date, sizeof(date), filename, sizeof(filename))) return UPLOAD_NONE;
  return uploadPhoto(date, filename, deadlineMs);
}

static bool applyCommand(JsonObject command, bool *capturedThisCycle, char *freshPhoto, size_t freshPhotoSize) {
  const char *id = command["id"] | "";
  const char *type = command["type"] | "";
  if (!id[0] || !type[0]) return false;
  String commandId(id);
  JsonObject payload = command["payload"].as<JsonObject>();

  String priorJson;
  if (sdStorageGetCloudCommand(id, priorJson)) {
    JsonDocument prior;
    if (deserializeJson(prior, priorJson)) return false;
    const char *priorState = prior["state"] | "uncertain";
    const char *priorMessage = prior["message"] | "";
    if (!strcmp(type, "take_photo") && strcmp(priorState, "failed") != 0) *capturedThisCycle = true;
    if (!strcmp(priorState, "started")) {
      // A reset may have happened during the side effect. Never repeat a photo
      // capture blindly; tell the backend that completion is uncertain.
      return persistCommandResult(commandId, "uncertain", "reset_during_command");
    }
    const char *photoDate = prior["photo"]["date"] | "";
    const char *photoName = prior["photo"]["filename"] | "";
    if (!strcmp(type, "take_photo") && photoDate[0] && photoName[0]) snprintf(freshPhoto, freshPhotoSize, "%s/%s", photoDate, photoName);
    return postCommandResult(commandId, priorState, priorMessage,
                             photoDate[0] && photoName[0] ? photoDate : NULL,
                             photoDate[0] && photoName[0] ? photoName : NULL);
  }
  if (!sdStorageSetCloudCommand(id, "{\"state\":\"started\"}")) {
    return postCommandResult(commandId, "failed", "sd_unavailable_for_command_journal");
  }

  if (!strcmp(type, "take_photo")) {
    char rel[48] = {0};
    bool ok = photoCaptureSnap(rel, sizeof(rel));
    if (!ok) return persistCommandResult(commandId, "failed", "capture_failed");
    *capturedThisCycle = true;
    snprintf(freshPhoto, freshPhotoSize, "%s", rel);
    char *slash = strchr(rel, '/');
    if (!slash) return persistCommandResult(commandId, "failed", "invalid_photo_path");
    *slash = '\0';
    return persistCommandResult(commandId, "completed", NULL, rel, slash + 1);
  }
  if (!strcmp(type, "stay_awake")) {
    stayAwake = true;
    return persistCommandResult(commandId, "completed", NULL);
  }
  if (!strcmp(type, "resume_sleep")) {
    stayAwake = false;
    maintenanceEndedEarly = true;
    return persistCommandResult(commandId, "completed", NULL);
  }
  if (!strcmp(type, "set_settings")) {
    if (payload["sleep_enabled"].is<bool>()) cloudDeviceSetSleepEnabled(payload["sleep_enabled"].as<bool>());
    if (payload["timer_enabled"].is<bool>()) captureTimerSetEnabled(payload["timer_enabled"].as<bool>());
    if (payload["interval_seconds"].is<uint32_t>()) captureTimerSetIntervalSeconds(payload["interval_seconds"].as<uint32_t>());
    uint16_t start = 0, end = 1439;
    bool hasStart = payloadMinute(payload, "window_start", &start);
    bool hasEnd = payloadMinute(payload, "window_end", &end);
    if (payload["window_enabled"].is<bool>() && hasStart && hasEnd) {
      captureTimerSetWindow(payload["window_enabled"].as<bool>(), start, end);
    }
    settingsSaveTimer(captureTimerIsEnabled(), captureTimerIntervalSeconds(), captureTimerWindowEnabled(), captureTimerWindowStart(), captureTimerWindowEnd());
    return persistCommandResult(commandId, "completed", NULL);
  }
  return persistCommandResult(commandId, "failed", "unsupported_command");
}

static bool sendSync(bool scheduledDue, bool actualAwake, bool processCommands, uint32_t deadlineMs,
                     bool *forceCapture, bool *capturedThisCycle, char *freshPhoto, size_t freshPhotoSize) {
  JsonDocument doc;
  char stationId[24];
  stationIdentityId(stationId, sizeof(stationId));
  doc["station_id"] = stationId;
  JsonObject status = doc["status"].to<JsonObject>();
  status["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  status["ip"] = WiFi.localIP().toString();
  status["sd_mounted"] = sdStorageIsMounted();
  status["sd_full"] = sdStorageIsFull();
  status["photo_count"] = sdStoragePhotoCount();
  status["camera_ok"] = photoCaptureCameraOk();
  status["timer_enabled"] = captureTimerIsEnabled();
  status["interval_seconds"] = captureTimerIntervalSeconds();
  status["window_enabled"] = captureTimerWindowEnabled();
  status["window_start"] = captureTimerWindowStart();
  status["window_end"] = captureTimerWindowEnd();
  status["in_window"] = captureTimerInWindow();
  status["scheduled_due"] = scheduledDue;
  status["time_set"] = timeKeeperIsSet();
  status["maintenance_remaining_seconds"] = cloudDeviceMaintenanceRemainingSeconds();
  status["upload_error_count"] = uploadErrors;
  status["last_upload_error"] = lastUploadError;
  status["cloud_enabled"] = configured;
  status["cloud_status_only"] = RICE_CLOUD_STATUS_ONLY != 0;
  status["cloud_upload_existing_photos"] = RICE_CLOUD_UPLOAD_EXISTING_PHOTOS != 0;
  status["sd_photo_protection"] = RICE_SD_PROTECT_PHOTOS != 0;
  status["sleep_enabled"] = cloudDeviceSleepEnabled();
  status["awake"] = actualAwake;
  status["stay_awake"] = stayAwake;
  time_t now = time(NULL);
  uint32_t until = captureTimerSecondsUntilNext();
  if (timeKeeperIsSet()) {
    uint32_t nextWake = (uint32_t)now + (until ? until : 600);
    doc["next_wake_at"] = nextWake;
    status["next_wake_at"] = nextWake;
  } else {
    doc["next_wake_at"] = nullptr;
    status["next_wake_at"] = nullptr;
  }
  doc["sleep_enabled"] = cloudDeviceSleepEnabled();
  doc["awake"] = actualAwake;
  String requestBody;
  serializeJson(doc, requestBody);
  String response;
  if (!cloudRequest("POST", "/api/device/sync", requestBody, &response)) return false;

  JsonDocument reply;
  if (deserializeJson(reply, response)) {
    uploadErrors++;
    snprintf(lastUploadError, sizeof(lastUploadError), "sync_parse_error");
    deviceLog("[cloud] sync reply parse failed");
    return false;
  }
  uint32_t serverTime = reply["server_time"] | 0U;
  if (serverTime) timeKeeperSetFromEpoch(serverTime, timeKeeperTzOffsetMinutes());
  *forceCapture = scheduledDue;
  JsonArray commands = reply["commands"].as<JsonArray>();
  if (!processCommands) return true;
  for (JsonObject command : commands) {
    if ((int32_t)(millis() - deadlineMs) >= 0) break;
    if (!applyCommand(command, capturedThisCycle, freshPhoto, freshPhotoSize)) deviceLog("[cloud] command result could not be acknowledged");
  }
  return true;
}

static void sleepUntilNextCycle(bool scheduledDue) {
  if (!cloudDeviceSleepEnabled() || stayAwake || otaActive || sdStorageIsErasing() || !captureTimerIsEnabled()) return;
  if (cloudDeviceMaintenanceRemainingSeconds() > 0) return;
  if (scheduledCyclePending || captureTimerConsumeDue()) {
    scheduledCyclePending = true;
    deviceLog("[cloud] pre-sleep barrier: capture due or pending, aborting sleep");
    return;
  }
  uint32_t seconds = captureTimerSecondsUntilNext();
  if (!seconds) return;
  uint32_t finalDeadline = millis() + 15000;
  bool ignoredCapture = false, ignoredDue = false;
  char ignoredPhoto[80] = {0};
  // Publish the actual transition to sleep and next wake time; leave any newly
  // returned commands pending for the next wake instead of silently discarding them.
  sendSync(false, false, false, finalDeadline, &ignoredDue, &ignoredCapture, ignoredPhoto, sizeof(ignoredPhoto));
  extern void stopCameraServer();
  extern void startCameraServer();
  stopCameraServer();  // drains active local HTTP handlers before power-down
  if (photoCaptureIsStreaming()) {
    startCameraServer();
    return;
  }
  // The HTTP server has stopped accepting requests; acquire and release the
  // shared SD mutex as a barrier so no photo handler or writer is still using
  // the card before powering down.
#if !RICE_CLOUD_STATUS_ONLY
  if (!sdStorageLock(20)) {
    startCameraServer();
    return;
  }
  sdStorageUnlock();
#endif
  if (scheduledCyclePending || captureTimerConsumeDue()) {
    scheduledCyclePending = true;
    startCameraServer();
    deviceLog("[cloud] pre-sleep barrier: capture due during shutdown, aborting sleep");
    return;
  }
  // The final sync and HTTP shutdown can take time. Recompute from the absolute
  // deadline immediately before sleep so that work time does not extend it.
  seconds = captureTimerSecondsUntilNext();
  if (!seconds) {
    startCameraServer();
    return;
  }
  if (seconds < 1) seconds = 1;
  photoCaptureSetTorch(false);
#if defined(LED_GPIO_NUM)
  gpio_reset_pin((gpio_num_t)LED_GPIO_NUM);
  pinMode(LED_GPIO_NUM, OUTPUT);
  digitalWrite(LED_GPIO_NUM, LOW);
  gpio_hold_en((gpio_num_t)LED_GPIO_NUM);
#endif
  // The camera driver owns GPIO32 as its power-down control on AI-Thinker.
  // Deinit releases it before sleep; external reset/power-on runs normal setup.
  if (photoCaptureCameraOk()) {
    esp_camera_deinit();
    photoCaptureSetCameraOk(false);
  }
#if defined(PWDN_GPIO_NUM) && PWDN_GPIO_NUM >= 0
  gpio_reset_pin((gpio_num_t)PWDN_GPIO_NUM);
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, HIGH);
  gpio_hold_en((gpio_num_t)PWDN_GPIO_NUM);
#endif
  gpio_deep_sleep_hold_en();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  deviceLog("[cloud] entering deep sleep for %lu seconds", (unsigned long)seconds);
  delay(30);
  esp_deep_sleep_start();
}

static void runCycle(bool scheduledDue) {
  if (!configured || otaActive) return;
  if (!cycleMutex) cycleMutex = xSemaphoreCreateMutex();
  if (!cycleMutex || xSemaphoreTake(cycleMutex, 0) != pdTRUE) return;
  cycleActive = true;
  if (scheduledCyclePending) {
    scheduledCyclePending = false;
    scheduledDue = true;
  }
  uint32_t deadline = millis() + CYCLE_BUDGET_MS;
  bool forceCapture = scheduledDue;
  bool capturedThisCycle = false;
  char freshPhoto[80] = {0};
  bool synced = WiFi.status() == WL_CONNECTED && sendSync(scheduledDue, true, RICE_CLOUD_STATUS_ONLY == 0, deadline, &forceCapture, &capturedThisCycle, freshPhoto, sizeof(freshPhoto));
  lastSyncOk = synced;
  if (synced) {
    consecutiveSyncFailures = 0;
  } else {
    consecutiveSyncFailures++;
  }

#if RICE_CLOUD_STATUS_ONLY
  deviceLog("[cloud] status-only sync %s; photo uploads and cloud commands disabled",
            synced ? "ok" : (lastUploadError[0] ? lastUploadError : "failed"));
  sleepUntilNextCycle(scheduledDue);
  cycleActive = false;
  xSemaphoreGive(cycleMutex);
  return;
#endif

  // If clock was unset upon wake (deep sleep timer wake), sendSync just initialized it.
  // Re-evaluate whether a scheduled capture is due now that wall-clock time is valid.
  if (synced && !forceCapture && !capturedThisCycle && timeKeeperIsSet()) {
    if (captureTimerConsumeDue()) {
      forceCapture = true;
      scheduledDue = true;
    }
  }

  if (forceCapture && !capturedThisCycle && photoCaptureCameraOk() && !sdStorageIsFull()) {
    // Scheduled timer and take_photo share one capture in a wake cycle.
    // A command already captured above satisfies this when it was the sole cause.
    if (scheduledDue) {
      char rel[48] = {0};
      if (photoCaptureSnap(rel, sizeof(rel))) {
        capturedThisCycle = true;
        snprintf(freshPhoto, sizeof(freshPhoto), "%s", rel);
        deviceLog("[cloud] scheduled capture saved %s", rel);
      }
    }
  }
  if (synced && freshPhoto[0] && (int32_t)(millis() - deadline) < 0 && cloudUploadBackoffReady(millis(), uploadBackoffActive, nextUploadAllowedMs)) {
    char *slash = strchr(freshPhoto, '/');
    if (slash) {
      *slash = '\0';
      UploadResult ur = uploadPhoto(freshPhoto, slash + 1, deadline);
      if (ur == UPLOAD_DONE) {
        consecutiveUploadFailures = 0;
      } else if (ur == UPLOAD_FAILED) {
        consecutiveUploadFailures++;
        uint32_t shift = consecutiveUploadFailures > 4 ? 4 : consecutiveUploadFailures;
        nextUploadAllowedMs = millis() + 15000UL * (1UL << shift);
        uploadBackoffActive = true;
      }
    }
  }
  while (synced && (int32_t)(millis() - deadline) < 0 && cloudUploadBackoffReady(millis(), uploadBackoffActive, nextUploadAllowedMs)) {
    if (WiFi.status() != WL_CONNECTED || otaActive) break;
    UploadResult result = uploadOnePendingPhoto(deadline);
    if (result == UPLOAD_DONE) {
      consecutiveUploadFailures = 0;
    } else if (result == UPLOAD_FAILED) {
      consecutiveUploadFailures++;
      uint32_t shift = consecutiveUploadFailures > 4 ? 4 : consecutiveUploadFailures;
      nextUploadAllowedMs = millis() + 15000UL * (1UL << shift);
      uploadBackoffActive = true;
      break;
    } else {
      break;
    }
    // Gentle upload pacing: yield to FreeRTOS IDLE/network tasks and local HTTP server
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  if (!synced) deviceLog("[cloud] sync failed: %s; unsent photos remain on SD", lastUploadError[0] ? lastUploadError : "error");

  // Recalculate remaining sleep time after upload work finishes.
  // If upload activity spanned past the scheduled capture deadline, handle it
  // cleanly with exactly one capture before sleeping, without a burst.
  if (timeKeeperIsSet() && captureTimerIsEnabled()) {
    uint32_t secondsUntil = captureTimerSecondsUntilNext();
    if (secondsUntil <= 1 && captureTimerConsumeDue()) {
      if (photoCaptureCameraOk() && !sdStorageIsFull()) {
        char overdueRel[48] = {0};
        if (photoCaptureSnap(overdueRel, sizeof(overdueRel))) {
          deviceLog("[cloud] post-upload due capture saved %s", overdueRel);
          if (synced && (int32_t)(millis() - deadline) < 0 && cloudUploadBackoffReady(millis(), uploadBackoffActive, nextUploadAllowedMs)) {
            char *slash = strchr(overdueRel, '/');
            if (slash) {
              *slash = '\0';
              UploadResult ur = uploadPhoto(overdueRel, slash + 1, deadline);
              if (ur == UPLOAD_DONE) {
                consecutiveUploadFailures = 0;
              } else if (ur == UPLOAD_FAILED) {
                consecutiveUploadFailures++;
                uint32_t shift = consecutiveUploadFailures > 4 ? 4 : consecutiveUploadFailures;
                nextUploadAllowedMs = millis() + 15000UL * (1UL << shift);
                uploadBackoffActive = true;
              }
            }
          }
        }
      }
    }
  }

  sleepUntilNextCycle(scheduledDue);
  cycleActive = false;
  xSemaphoreGive(cycleMutex);
}

void cloudDeviceBegin() {
  configured = cloudConfiguredFromHeaders();
  if (configured && psramFound()) {
    int result = mbedtls_platform_set_calloc_free(cloudTlsCalloc, cloudTlsFree);
    deviceLog("[cloud] PSRAM-first TLS allocator: %s; PSRAM=%u",
              result == 0 ? "ready" : "unavailable", (unsigned)ESP.getFreePsram());
  }
  bootMs = millis();
  cycleMutex = xSemaphoreCreateMutex();
  if (!configured) deviceLog("[cloud] disabled: cloud_secrets.h needs a station token and root CA");
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  wakeCyclePending = configured && sleepEnabledStored() && cause == ESP_SLEEP_WAKEUP_TIMER;
}

uint32_t cloudDeviceMaintenanceRemainingSeconds() {
  if (maintenanceEndedEarly || esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) return 0;
  uint32_t elapsed = (millis() - bootMs) / 1000;
  return elapsed >= MAINTENANCE_SECONDS ? 0 : MAINTENANCE_SECONDS - elapsed;
}

void cloudDeviceScheduledWake() {
  // Called by the small capture timer task. Defer HTTPS, hashing, and upload
  // work to Arduino loopTask, which has an explicitly enlarged stack.
  scheduledCyclePending = true;
}

bool cloudDeviceTimerWakePending() { return wakeCyclePending; }

void cloudDeviceLoop() {
  if (!configured || otaActive) return;
#if RICE_CLOUD_STATUS_ONLY
  // Keep local OTA recovery responsive while Wi-Fi/NTP settles after reboot.
  if (millis() - bootMs < 30000UL) return;
#endif
  if (scheduledCyclePending) {
    scheduledCyclePending = false;
    wakeCyclePending = false;
    runCycle(true);
    return;
  }
  if (wakeCyclePending) {
    bool due = captureTimerConsumeDue();
    wakeCyclePending = false;
    // A timer wake polls commands, but only captures when the configured
    // schedule currently allows one (for example, after a wrapped window).
    runCycle(due);
    return;
  }
  if (WiFi.status() != WL_CONNECTED && !cloudDeviceSleepEnabled()) return;

  uint32_t baseInterval = 60000;
  if (cloudDeviceMaintenanceRemainingSeconds() > 0 || stayAwake) {
    baseInterval = 5000;
  } else if (!cloudDeviceSleepEnabled()) {
    baseInterval = 30000; // Continuous mode base sync: 30s
  }

  // Gentle backoff on consecutive sync/network failures or upload failures:
  // 30s -> 60s -> 120s -> 240s -> max 300s
  uint32_t failures = consecutiveSyncFailures > consecutiveUploadFailures ? consecutiveSyncFailures : consecutiveUploadFailures;
  uint32_t interval = baseInterval;
  if (failures > 0) {
    uint32_t shift = failures > 4 ? 4 : failures;
    interval = baseInterval * (1UL << shift);
    if (interval > 300000UL) interval = 300000UL;
  }

  if (!lastPeriodicSyncMs || millis() - lastPeriodicSyncMs > interval) {
    lastPeriodicSyncMs = millis();
    runCycle(false);
  }
}
