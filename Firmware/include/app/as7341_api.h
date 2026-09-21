#pragma once
#include <Adafruit_AS7341.h>
#include "app/spectrometer_types.h"

bool initAS7341();
bool as7341_readAndValidateChipId(uint8_t *raw_out);
bool as7341_readInto(SpectrometerResult *out);
bool as7341_setAtIME(uint8_t atime_value);
uint8_t as7341_getAtIME();
bool as7341_setAStep(uint16_t astep_value);
uint16_t as7341_getAStep();
bool as7341_setGain(as7341_gain_t gain);
uint8_t as7341_getGain();
uint16_t as7341_setLEDCurrent(uint16_t led_current_ma);

// ADC full scale for the current ATIME/ASTEP — not a fixed 0xFFFF.
uint16_t as7341_getFullScale();

// Raw status bytes captured during the last as7341_readInto(), for `spec_diag` bringup:
// the latched STATUS (0x93) plus STATUS2 (0xA3) sampled after each SMUX pass.
void as7341_getLastStatusBytes(uint8_t *status, uint8_t *status2_low, uint8_t *status2_high);
bool as7341_readDiagRegister(uint8_t which, uint8_t *value);
uint8_t as7341_intenabRegister();
