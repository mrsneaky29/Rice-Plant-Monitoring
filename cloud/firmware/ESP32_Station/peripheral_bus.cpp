#include "peripheral_bus.h"
#include <Arduino.h>
#include <Wire.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "board_config.h"
#include "device_log.h"
#include "driver/gpio.h"

#ifndef PERIPH_I2C_SDA
#define PERIPH_I2C_SDA 3
#endif
#ifndef PERIPH_I2C_SCL
#define PERIPH_I2C_SCL 1
#endif
#ifndef PERIPH_I2C_FREQ_HZ
#define PERIPH_I2C_FREQ_HZ 100000
#endif

#if defined(CAMERA_MODEL_AI_THINKER)
#if PERIPH_I2C_SDA == 16 || PERIPH_I2C_SDA == 17 || PERIPH_I2C_SCL == 16 || PERIPH_I2C_SCL == 17
#error "GPIO 16/17 are PSRAM on AI-Thinker ESP32-CAM"
#endif
#endif

static SemaphoreHandle_t busMutex = NULL;
static bool busReady = false;

bool peripheralBusBegin() {
  if (busReady) {
    return true;
  }
  busMutex = xSemaphoreCreateMutex();
  if (!busMutex) {
    return false;
  }

  gpio_reset_pin((gpio_num_t)PERIPH_I2C_SDA);
  gpio_reset_pin((gpio_num_t)PERIPH_I2C_SCL);
  Wire.begin(PERIPH_I2C_SDA, PERIPH_I2C_SCL, (uint32_t)PERIPH_I2C_FREQ_HZ);
  busReady = true;
  deviceLog("I2C ready on SDA=GPIO%d (RX0) SCL=GPIO%d (TX0) @ %d Hz -- unplug ADS1115 when flashing USB",
            PERIPH_I2C_SDA, PERIPH_I2C_SCL, PERIPH_I2C_FREQ_HZ);

  if (xSemaphoreTake(busMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    int found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        deviceLog("  I2C device at 0x%02X", addr);
        found++;
      }
    }
    if (found == 0) {
      deviceLog("  (no I2C devices seen -- check 3.3V, GND, SDA=RX0, SCL=TX0)");
    }
    xSemaphoreGive(busMutex);
  }
  return true;
}

bool peripheralBusReady() {
  return busReady;
}

bool peripheralBusWriteReg8(uint8_t devAddr, uint8_t reg, uint8_t value) {
  if (!busReady || xSemaphoreTake(busMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    return false;
  }
  Wire.beginTransmission(devAddr);
  Wire.write(reg);
  Wire.write(value);
  bool ok = Wire.endTransmission() == 0;
  xSemaphoreGive(busMutex);
  return ok;
}

bool peripheralBusReadReg8(uint8_t devAddr, uint8_t reg, uint8_t *outValue) {
  if (!busReady || !outValue || xSemaphoreTake(busMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    return false;
  }
  Wire.beginTransmission(devAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    xSemaphoreGive(busMutex);
    return false;
  }
  bool ok = Wire.requestFrom((int)devAddr, 1) == 1;
  if (ok) {
    *outValue = (uint8_t)Wire.read();
  }
  xSemaphoreGive(busMutex);
  return ok;
}

bool peripheralBusWriteReg16(uint8_t devAddr, uint8_t reg, uint16_t value) {
  if (!busReady || xSemaphoreTake(busMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    return false;
  }
  Wire.beginTransmission(devAddr);
  Wire.write(reg);
  Wire.write((uint8_t)(value >> 8));
  Wire.write((uint8_t)(value & 0xFF));
  bool ok = Wire.endTransmission() == 0;
  xSemaphoreGive(busMutex);
  return ok;
}

bool peripheralBusReadReg16(uint8_t devAddr, uint8_t reg, uint16_t *outValue) {
  if (!busReady || !outValue || xSemaphoreTake(busMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    return false;
  }
  Wire.beginTransmission(devAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    xSemaphoreGive(busMutex);
    return false;
  }
  bool ok = Wire.requestFrom((int)devAddr, 2) == 2;
  if (ok) {
    uint16_t hi = (uint16_t)Wire.read();
    uint16_t lo = (uint16_t)Wire.read();
    *outValue = (uint16_t)((hi << 8) | lo);
  }
  xSemaphoreGive(busMutex);
  return ok;
}
