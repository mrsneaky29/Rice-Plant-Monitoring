#include "water_level.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "mbedtls/base64.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sd_storage.h"
#include "water_level_storage.h"
#include "settings_store.h"
#include "device_log.h"

// The Responses API accepts image input and schema-constrained output, which
// keeps every saved analysis in a predictable format for the dashboard.
#define OPENAI_MODEL "gpt-4o-mini"
#define OPENAI_URL "https://api.openai.com/v1/responses"
#define ANALYSIS_QUEUE_LEN 8
#define ANALYSIS_RETRY_DELAY_MS 5000
#define ANALYSIS_MAX_RETRIES 60

struct AnalysisJob {
  char date[16];
  char name[40];
  bool manual;
  uint32_t generation;
};

struct AnalysisReservation {
  bool used;
  char date[16];
  char name[40];
};

struct PlantAnalysis {
  float waterLevelCm;
  int tillerCount;
  char growthStage[48];
  float waterConfidence;
  float tillerConfidence;
  float growthConfidence;
  char notes[128];
};

static QueueHandle_t analysisQueue = NULL;
static volatile bool visionEnabled = false;
static volatile uint32_t analysisGeneration = 1;
static portMUX_TYPE reservationMux = portMUX_INITIALIZER_UNLOCKED;
static AnalysisReservation reservations[ANALYSIS_QUEUE_LEN + 1] = {};

enum AnalysisOutcome {
  ANALYSIS_COMPLETE,
  ANALYSIS_RETRY,
  ANALYSIS_CANCELLED,
};

void waterLevelSetVisionEnabled(bool enabled) {
  visionEnabled = enabled;
}

bool waterLevelVisionEnabled() {
  return visionEnabled;
}

static bool validJobPart(const char *value, size_t maxLen) {
  return value && value[0] && strlen(value) <= maxLen &&
         !strstr(value, "..") && !strchr(value, '/') && !strchr(value, '\\');
}

static WaterLevelQueueResult reserveAnalysis(const char *date, const char *name) {
  int freeSlot = -1;
  portENTER_CRITICAL(&reservationMux);
  for (unsigned i = 0; i < sizeof(reservations) / sizeof(reservations[0]); i++) {
    if (reservations[i].used && strcmp(reservations[i].date, date) == 0 && strcmp(reservations[i].name, name) == 0) {
      portEXIT_CRITICAL(&reservationMux);
      return WATER_LEVEL_QUEUE_ALREADY_PENDING;
    }
    if (!reservations[i].used && freeSlot < 0) freeSlot = (int)i;
  }
  if (freeSlot >= 0) {
    reservations[freeSlot].used = true;
    snprintf(reservations[freeSlot].date, sizeof(reservations[freeSlot].date), "%s", date);
    snprintf(reservations[freeSlot].name, sizeof(reservations[freeSlot].name), "%s", name);
  }
  portEXIT_CRITICAL(&reservationMux);
  return freeSlot >= 0 ? WATER_LEVEL_QUEUE_QUEUED : WATER_LEVEL_QUEUE_FULL;
}

static void releaseAnalysis(const char *date, const char *name) {
  portENTER_CRITICAL(&reservationMux);
  for (unsigned i = 0; i < sizeof(reservations) / sizeof(reservations[0]); i++) {
    if (reservations[i].used && strcmp(reservations[i].date, date) == 0 && strcmp(reservations[i].name, name) == 0) {
      reservations[i].used = false;
      break;
    }
  }
  portEXIT_CRITICAL(&reservationMux);
}

static WaterLevelQueueResult enqueueAnalysis(const char *date, const char *name, bool manual) {
  if (!analysisQueue) return WATER_LEVEL_QUEUE_UNAVAILABLE;
  if (!validJobPart(date, 15) || !validJobPart(name, 39)) {
    return WATER_LEVEL_QUEUE_INVALID;
  }
  WaterLevelQueueResult reserved = reserveAnalysis(date, name);
  if (reserved != WATER_LEVEL_QUEUE_QUEUED) return reserved;

  AnalysisJob job;
  snprintf(job.date, sizeof(job.date), "%s", date);
  snprintf(job.name, sizeof(job.name), "%s", name);
  job.manual = manual;
  job.generation = analysisGeneration;
  if (xQueueSend(analysisQueue, &job, 0) != pdTRUE) {
    releaseAnalysis(job.date, job.name);
    deviceLog("[analysis] queue full; skipped %s/%s", job.date, job.name);
    return WATER_LEVEL_QUEUE_FULL;
  }
  deviceLog("[analysis] %s request queued for %s/%s", manual ? "manual" : "automatic", job.date, job.name);
  return WATER_LEVEL_QUEUE_QUEUED;
}

void waterLevelEnqueueAnalysis(const char *date, const char *name) {
  if (!visionEnabled) return;
  enqueueAnalysis(date, name, false);
}

WaterLevelQueueResult waterLevelEnqueueManualAnalysis(const char *date, const char *name) {
  return enqueueAnalysis(date, name, true);
}

void waterLevelCancelAll() {
  analysisGeneration++;
  if (analysisQueue) xQueueReset(analysisQueue);
  portENTER_CRITICAL(&reservationMux);
  memset(reservations, 0, sizeof(reservations));
  portEXIT_CRITICAL(&reservationMux);
}

static const char *REQUEST_PREFIX =
  "{\"model\":\"" OPENAI_MODEL "\",\"store\":false,\"max_output_tokens\":350,"
  "\"input\":[{\"role\":\"user\",\"content\":["
  "{\"type\":\"input_text\",\"text\":\"Analyze this photograph of one rice plant. "
  "Measure the water level in centimeters only when a graduated metric ruler or scale has visible tick marks or numbers and a visible waterline crossing. "
  "Count rice tillers emerging from the plant base; count stems, not leaves. "
  "Give a short rice growth stage. If any value cannot be determined from visible evidence, return null for that value and a low confidence. "
  "Do not guess hidden tillers or an unreadable waterline.\"},"
  "{\"type\":\"input_image\",\"image_url\":\"data:image/jpeg;base64,";

static const char *REQUEST_SUFFIX =
  "\",\"detail\":\"high\"}]}],"
  "\"text\":{\"format\":{\"type\":\"json_schema\",\"name\":\"rice_plant_analysis\",\"strict\":true,"
  "\"schema\":{\"type\":\"object\",\"additionalProperties\":false,"
  "\"properties\":{"
  "\"water_level_cm\":{\"type\":[\"number\",\"null\"],\"minimum\":0},"
  "\"tiller_count\":{\"type\":[\"integer\",\"null\"],\"minimum\":0},"
  "\"growth_stage\":{\"type\":[\"string\",\"null\"]},"
  "\"water_confidence\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1},"
  "\"tiller_confidence\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1},"
  "\"growth_confidence\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1},"
  "\"notes\":{\"type\":\"string\"}},"
  "\"required\":[\"water_level_cm\",\"tiller_count\",\"growth_stage\",\"water_confidence\",\"tiller_confidence\",\"growth_confidence\",\"notes\"]}}}}";

static float clampConfidence(float value) {
  if (isnan(value) || value < 0) return 0;
  if (value > 1) return 1;
  return value;
}

static char *buildRequestBody(const uint8_t *jpg, size_t jpgLen, size_t *outBodyLen) {
  size_t prefixLen = strlen(REQUEST_PREFIX);
  size_t suffixLen = strlen(REQUEST_SUFFIX);
  size_t b64Cap = 4 * ((jpgLen + 2) / 3) + 4;

  char *body = (char *)heap_caps_malloc(prefixLen + b64Cap + suffixLen + 1, MALLOC_CAP_SPIRAM);
  if (!body) return NULL;
  memcpy(body, REQUEST_PREFIX, prefixLen);

  size_t b64ActualLen = 0;
  int rc = mbedtls_base64_encode((unsigned char *)(body + prefixLen), b64Cap, &b64ActualLen, jpg, jpgLen);
  if (rc != 0) {
    free(body);
    return NULL;
  }

  memcpy(body + prefixLen + b64ActualLen, REQUEST_SUFFIX, suffixLen + 1);
  *outBodyLen = prefixLen + b64ActualLen + suffixLen;
  return body;
}

static bool parseModelReply(const char *content, PlantAnalysis *out) {
  if (!content || !out) return false;
  JsonDocument result;
  if (deserializeJson(result, content)) return false;
  if (!result.is<JsonObject>()) return false;
  JsonObject obj = result.as<JsonObject>();
  const char *required[] = {
    "water_level_cm", "tiller_count", "growth_stage", "water_confidence",
    "tiller_confidence", "growth_confidence", "notes"
  };
  for (const char *key : required) {
    if (!obj.containsKey(key)) return false;
  }

  JsonVariant water = obj["water_level_cm"];
  JsonVariant tillers = obj["tiller_count"];
  JsonVariant stageValue = obj["growth_stage"];
  if (!water.isNull() && !water.is<float>() && !water.is<int>()) return false;
  if (!tillers.isNull() && !tillers.is<int>()) return false;
  if (!stageValue.isNull() && !stageValue.is<const char *>()) return false;
  if (!obj["water_confidence"].is<float>() && !obj["water_confidence"].is<int>()) return false;
  if (!obj["tiller_confidence"].is<float>() && !obj["tiller_confidence"].is<int>()) return false;
  if (!obj["growth_confidence"].is<float>() && !obj["growth_confidence"].is<int>()) return false;
  if (!obj["notes"].is<const char *>()) return false;

  out->waterLevelCm = water.isNull() ? NAN : water.as<float>();
  out->tillerCount = tillers.isNull() ? -1 : tillers.as<int>();
  if ((!isnan(out->waterLevelCm) && (!isfinite(out->waterLevelCm) || out->waterLevelCm < 0)) || out->tillerCount < -1) return false;

  const char *stage = stageValue.isNull() ? "" : stageValue.as<const char *>();
  const char *notes = obj["notes"].as<const char *>();
  snprintf(out->growthStage, sizeof(out->growthStage), "%s", stage ? stage : "");
  snprintf(out->notes, sizeof(out->notes), "%s", notes ? notes : "");
  float rawWaterConfidence = obj["water_confidence"].as<float>();
  float rawTillerConfidence = obj["tiller_confidence"].as<float>();
  float rawGrowthConfidence = obj["growth_confidence"].as<float>();
  if (!isfinite(rawWaterConfidence) || rawWaterConfidence < 0 || rawWaterConfidence > 1 ||
      !isfinite(rawTillerConfidence) || rawTillerConfidence < 0 || rawTillerConfidence > 1 ||
      !isfinite(rawGrowthConfidence) || rawGrowthConfidence < 0 || rawGrowthConfidence > 1) return false;
  out->waterConfidence = clampConfidence(rawWaterConfidence);
  out->tillerConfidence = clampConfidence(rawTillerConfidence);
  out->growthConfidence = clampConfidence(rawGrowthConfidence);
  return true;
}

static const char *findOutputText(JsonDocument &response, char *refusal, size_t refusalSize) {
  if (refusal && refusalSize) refusal[0] = '\0';
  JsonArray output = response["output"].as<JsonArray>();
  for (JsonObject item : output) {
    const char *itemType = item["type"] | "";
    if (strcmp(itemType, "message") != 0) continue;
    JsonArray content = item["content"].as<JsonArray>();
    for (JsonObject part : content) {
      const char *type = part["type"] | "";
      if (strcmp(type, "refusal") == 0) {
        const char *message = part["refusal"] | "Request refused";
        if (refusal && refusalSize) snprintf(refusal, refusalSize, "%s", message);
        continue;
      }
      if (strcmp(type, "output_text") == 0) {
        const char *text = part["text"] | (const char *)NULL;
        if (text) return text;
      }
    }
  }
  return NULL;
}

static bool readPhotoIntoBuffer(const char *date, const char *name, uint8_t **outBuf, size_t *outLen) {
  if (!sdStorageLock(3000)) return false;
  File f = sdStorageOpenPhoto(date, name);
  if (!f) {
    sdStorageUnlock();
    return false;
  }
  size_t len = f.size();
  uint8_t *buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!buf) {
    f.close();
    sdStorageUnlock();
    return false;
  }
  size_t n = f.read(buf, len);
  f.close();
  sdStorageUnlock();
  if (n != len) {
    free(buf);
    return false;
  }
  *outBuf = buf;
  *outLen = len;
  return true;
}

static AnalysisOutcome analyzeOne(const AnalysisJob &job) {
  uint8_t *jpg = NULL;
  size_t jpgLen = 0;
  char apiKey[OPENAI_API_KEY_MAX];

  if (job.generation != analysisGeneration || (!job.manual && !visionEnabled)) {
    return ANALYSIS_CANCELLED;
  }
  if (WiFi.status() != WL_CONNECTED) {
    deviceLog("[analysis] waiting for WiFi: %s/%s", job.date, job.name);
    return ANALYSIS_RETRY;
  }
  if (!settingsGetOpenAiKey(apiKey, sizeof(apiKey))) {
    deviceLog("[analysis] waiting for API key: %s/%s", job.date, job.name);
    return ANALYSIS_RETRY;
  }
  if (!readPhotoIntoBuffer(job.date, job.name, &jpg, &jpgLen)) {
    deviceLog("[analysis] failed %s/%s: could not read photo", job.date, job.name);
    return ANALYSIS_COMPLETE;
  }

  size_t bodyLen = 0;
  char *body = buildRequestBody(jpg, jpgLen, &bodyLen);
  free(jpg);
  if (!body) {
    deviceLog("[analysis] failed %s/%s: insufficient PSRAM for request", job.date, job.name);
    return ANALYSIS_RETRY;
  }
  if (job.generation != analysisGeneration || (!job.manual && !visionEnabled)) {
    free(body);
    return ANALYSIS_CANCELLED;
  }

  WiFiClientSecure client;
  client.setInsecure();  // Pin a CA certificate before exposing this device to an untrusted network.
  client.setTimeout(30000);

  HTTPClient https;
  https.setConnectTimeout(10000);
  https.setTimeout(45000);
  if (!https.begin(client, OPENAI_URL)) {
    deviceLog("[analysis] failed %s/%s: HTTPS connection setup failed", job.date, job.name);
    free(body);
    return ANALYSIS_RETRY;
  }
  https.addHeader("Content-Type", "application/json");
  char authHeader[OPENAI_API_KEY_MAX + 16];
  snprintf(authHeader, sizeof(authHeader), "Bearer %s", apiKey);
  https.addHeader("Authorization", authHeader);

  int httpCode = https.POST((uint8_t *)body, bodyLen);
  free(body);
  String responseBody = https.getString();
  https.end();

  if (job.generation != analysisGeneration || (!job.manual && !visionEnabled)) {
    return ANALYSIS_CANCELLED;
  }
  if (httpCode != HTTP_CODE_OK) {
    char logTag[72];
    snprintf(logTag, sizeof(logTag), "analysis_error %s/%s HTTP %d", job.date, job.name, httpCode);
    deviceLogRaw(logTag, responseBody.c_str());
    deviceLog("[analysis] failed %s/%s: OpenAI HTTP %d", job.date, job.name, httpCode);
    return (httpCode < 0 || httpCode == 408 || httpCode == 429 || httpCode >= 500)
             ? ANALYSIS_RETRY
             : ANALYSIS_COMPLETE;
  }

  // Keep only fields used below so the parsed document stays small even if
  // the API adds metadata to the response.
  JsonDocument responseFilter;
  responseFilter["status"] = true;
  responseFilter["incomplete_details"]["reason"] = true;
  responseFilter["error"]["message"] = true;
  responseFilter["output"][0]["type"] = true;
  responseFilter["output"][0]["content"][0]["type"] = true;
  responseFilter["output"][0]["content"][0]["text"] = true;
  responseFilter["output"][0]["content"][0]["refusal"] = true;

  JsonDocument response;
  DeserializationError responseError = deserializeJson(
    response, responseBody, DeserializationOption::Filter(responseFilter)
  );
  if (responseError) {
    deviceLog("[analysis] failed %s/%s: response parse error: %s", job.date, job.name, responseError.c_str());
    return ANALYSIS_COMPLETE;
  }

  const char *status = response["status"] | "";
  if (strcmp(status, "completed") != 0) {
    const char *reason = response["incomplete_details"]["reason"] | (const char *)NULL;
    const char *errorMessage = response["error"]["message"] | (const char *)NULL;
    deviceLog("[analysis] failed %s/%s: response status=%s reason=%s", job.date, job.name,
              status[0] ? status : "unknown", reason ? reason : (errorMessage ? errorMessage : "unspecified"));
    return ANALYSIS_COMPLETE;
  }

  char refusal[128];
  const char *outputText = findOutputText(response, refusal, sizeof(refusal));
  if (refusal[0]) {
    deviceLog("[analysis] refused %s/%s: %s", job.date, job.name, refusal);
    return ANALYSIS_COMPLETE;
  }
  PlantAnalysis result;
  if (!outputText || !parseModelReply(outputText, &result)) {
    deviceLog("[analysis] failed %s/%s: structured output missing or invalid", job.date, job.name);
    return ANALYSIS_COMPLETE;
  }

  if (!waterLevelStorageAppend(
        job.date, result.waterLevelCm, result.tillerCount, result.growthStage,
        result.waterConfidence, result.tillerConfidence, result.growthConfidence,
        job.name, result.notes)) {
    deviceLog("[analysis] succeeded for %s/%s but SD save failed", job.date, job.name);
    return ANALYSIS_RETRY;
  }

  char waterText[24], tillerText[16];
  if (isnan(result.waterLevelCm)) snprintf(waterText, sizeof(waterText), "unreadable");
  else snprintf(waterText, sizeof(waterText), "%.1fcm", result.waterLevelCm);
  if (result.tillerCount < 0) snprintf(tillerText, sizeof(tillerText), "unreadable");
  else snprintf(tillerText, sizeof(tillerText), "%d", result.tillerCount);
  deviceLog("[analysis] %s/%s: water=%s tillers=%s stage=%s", job.date, job.name, waterText, tillerText, result.growthStage);
  return ANALYSIS_COMPLETE;
}

static void analysisTask(void *pvParameters) {
  AnalysisJob job;
  for (;;) {
    if (xQueueReceive(analysisQueue, &job, portMAX_DELAY) != pdTRUE) continue;

    uint8_t attempts = 0;
    for (;;) {
      AnalysisOutcome outcome = analyzeOne(job);
      if (outcome != ANALYSIS_RETRY) {
        if (outcome == ANALYSIS_CANCELLED) {
          deviceLog("[analysis] cancelled %s/%s", job.date, job.name);
        }
        break;
      }

      attempts++;
      if (attempts >= ANALYSIS_MAX_RETRIES) {
        deviceLog("[analysis] gave up after %u retries: %s/%s", (unsigned)attempts, job.date, job.name);
        break;
      }
      if (attempts == 1 || attempts % 12 == 0) {
        deviceLog("[analysis] retrying %s/%s (attempt %u/%u)", job.date, job.name,
                  (unsigned)attempts, (unsigned)ANALYSIS_MAX_RETRIES);
      }
      vTaskDelay(pdMS_TO_TICKS(ANALYSIS_RETRY_DELAY_MS));
    }
    releaseAnalysis(job.date, job.name);
  }
}

void waterLevelBegin() {
  analysisQueue = xQueueCreate(ANALYSIS_QUEUE_LEN, sizeof(AnalysisJob));
  if (!analysisQueue) {
    deviceLog("[analysis] queue allocation failed; AI vision is unavailable");
    return;
  }
  if (xTaskCreatePinnedToCore(analysisTask, "plantVision", 14336, NULL, 1, NULL, 1) != pdPASS) {
    deviceLog("[analysis] task creation failed; AI vision is unavailable");
    vQueueDelete(analysisQueue);
    analysisQueue = NULL;
  }
}
