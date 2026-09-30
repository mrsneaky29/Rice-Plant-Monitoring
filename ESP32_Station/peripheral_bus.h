#ifndef PERIPHERAL_BUS_H
#define PERIPHERAL_BUS_H

#include <stdint.h>
#include <stdbool.h>

bool peripheralBusBegin();
bool peripheralBusReady();
bool peripheralBusWriteReg8(uint8_t devAddr, uint8_t reg, uint8_t value);
bool peripheralBusReadReg8(uint8_t devAddr, uint8_t reg, uint8_t *outValue);
bool peripheralBusWriteReg16(uint8_t devAddr, uint8_t reg, uint16_t value);
bool peripheralBusReadReg16(uint8_t devAddr, uint8_t reg, uint16_t *outValue);

#endif
