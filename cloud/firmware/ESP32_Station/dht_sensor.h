#ifndef DHT_SENSOR_H
#define DHT_SENSOR_H

#include <stdint.h>

void dhtSensorBegin();

// Blocking read (a few ms). Returns false on a checksum/timeout failure --
// DHT22 reads are flaky by nature; callers should just skip a failed
// reading rather than retry in a tight loop (the sensor needs >=2s between
// reads regardless).
bool dhtSensorRead(float *outTempC, float *outHumidity);

// Most recent successful reading, cached in RAM (for status/UI display
// without forcing a fresh read on every poll).
bool dhtSensorLastReading(float *outTempC, float *outHumidity, uint32_t *outAgeSeconds);

#endif  // DHT_SENSOR_H
