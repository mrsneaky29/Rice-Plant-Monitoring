#ifndef ADS1115_H
#define ADS1115_H

#include <stdint.h>
#include <stdbool.h>

bool ads1115Begin();
bool ads1115Present();
uint8_t ads1115Address();
bool ads1115ReadA0(float *outVolts);
float ads1115MoisturePercent(float volts);

#endif
