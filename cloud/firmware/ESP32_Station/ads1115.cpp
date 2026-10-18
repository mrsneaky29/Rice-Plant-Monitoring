#include "ads1115.h"
#include <Arduino.h>
#include "peripheral_bus.h"
#include "device_log.h"

#define ADS1115_REG_CONV 0x00
#define ADS1115_REG_CFG 0x01
#define ADS1115_PGA_4V096 0.000125f
#define ADS1115_CFG_A0_GND 0xC383

static uint8_t chipAddr = 0;
static bool present = false;

bool ads1115Begin() {
  present = false;
  chipAddr = 0;
  static const uint8_t candidates[] = { 0x48, 0x49, 0x4A, 0x4B };
  for (unsigned i = 0; i < sizeof(candidates); i++) {
    uint16_t cfg = 0;
    if (peripheralBusReadReg16(candidates[i], ADS1115_REG_CFG, &cfg)) {
      chipAddr = candidates[i];
      present = true;
      deviceLog("ADS1115 found at 0x%02X -- using A0 only", chipAddr);
      float v = 0;
      if (ads1115ReadA0(&v)) {
        deviceLog("  A0 = %.3f V  moisture=%.0f%% (wet=low V)", v, ads1115MoisturePercent(v));
      }
      return true;
    }
  }
  deviceLog("ADS1115 not found on SDA=RX0/GPIO3 SCL=TX0/GPIO1 (tried 0x48-0x4B)");
  return false;
}

bool ads1115Present() {
  return present;
}

uint8_t ads1115Address() {
  return chipAddr;
}

bool ads1115ReadA0(float *outVolts) {
  if (!present || !outVolts) {
    return false;
  }
  if (!peripheralBusWriteReg16(chipAddr, ADS1115_REG_CFG, ADS1115_CFG_A0_GND)) {
    present = false;
    deviceLog("ADS1115 write failed -- mark missing");
    return false;
  }
  delay(12);
  uint16_t raw = 0;
  if (!peripheralBusReadReg16(chipAddr, ADS1115_REG_CONV, &raw)) {
    return false;
  }
  float volts = (float)((int16_t)raw) * ADS1115_PGA_4V096;
  *outVolts = volts;
  return true;
}

float ads1115MoisturePercent(float volts) {
  const float vDry = 3.30f;
  const float vWet = 0.00f;
  float pct = (vDry - volts) * (100.0f / (vDry - vWet));
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}
