#include "dht_sensor.h"
#include <Arduino.h>
#include "device_log.h"
#include "settings_store.h"

#define DHT22_PIN 13

static float lastTempC = NAN;
static float lastHumidity = NAN;
static uint32_t lastReadMs = 0;
static bool haveReading = false;

void dhtSensorBegin() {
  if (settingsStationRole() != STATION_ROLE_DHT22) {
    deviceLog("DHT22 inactive for this station profile");
    return;
  }
  pinMode(DHT22_PIN, INPUT_PULLUP);
  deviceLog("DHT22 ready on GPIO%d", DHT22_PIN);
}

bool dhtSensorRead(float *outTempC, float *outHumidity) {
  if (settingsStationRole() != STATION_ROLE_DHT22) return false;
  static uint32_t lastAttempt = 0;
  if (lastAttempt && millis() - lastAttempt < 2100) return false;
  lastAttempt = millis();

  uint8_t data[5] = {0};
  pinMode(DHT22_PIN, OUTPUT);
  digitalWrite(DHT22_PIN, LOW);
  delay(2);
  digitalWrite(DHT22_PIN, HIGH);
  delayMicroseconds(30);
  pinMode(DHT22_PIN, INPUT_PULLUP);

  if (!pulseIn(DHT22_PIN, LOW, 120) || !pulseIn(DHT22_PIN, HIGH, 120)) return false;
  for (int bit = 0; bit < 40; bit++) {
    if (!pulseIn(DHT22_PIN, LOW, 120)) return false;
    unsigned long highUs = pulseIn(DHT22_PIN, HIGH, 150);
    if (!highUs) return false;
    data[bit / 8] <<= 1;
    if (highUs > 45) data[bit / 8] |= 1;
  }
  if ((uint8_t)(data[0] + data[1] + data[2] + data[3]) != data[4]) return false;
  float h = ((uint16_t)data[0] << 8 | data[1]) / 10.0f;
  int16_t rawT = ((uint16_t)(data[2] & 0x7F) << 8) | data[3];
  float t = rawT / 10.0f;
  if (data[2] & 0x80) t = -t;
  if (h < 0 || h > 100 || t < -40 || t > 80) return false;

  lastTempC = t;
  lastHumidity = h;
  lastReadMs = millis();
  haveReading = true;
  if (outTempC) *outTempC = t;
  if (outHumidity) *outHumidity = h;
  return true;
}

bool dhtSensorLastReading(float *outTempC, float *outHumidity, uint32_t *outAgeSeconds) {
  if (!haveReading) {
    return false;
  }
  if (outTempC) *outTempC = lastTempC;
  if (outHumidity) *outHumidity = lastHumidity;
  if (outAgeSeconds) *outAgeSeconds = (millis() - lastReadMs) / 1000;
  return true;
}
