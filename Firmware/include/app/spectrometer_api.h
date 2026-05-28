#pragma once

#include <Arduino.h>
#include <stdint.h>
#include "app/spectrometer_types.h"

extern bool spectrometer_available;
extern SpectrometerModel spectrometer_model;

extern float par_coefficients[18]; // per-channel PAR conversion coefficients, indexed 0..channel_count-1

const char *spectrometerModelName(SpectrometerModel model);
bool initSpectrometer();
bool spectrometerPrepareLegacyCommand();
bool spectrometer_read();
bool spectrometer_read_raw();
bool spectrometer_set_led_current(uint16_t led_current_ma);
bool spectrometer_read_flash(uint16_t led_current_ma);

// Raw-helper accessors for non-JSON command output
bool spectrometerReadInto(SpectrometerResult *out);
uint16_t spectrometerSetLedCurrentSilent(uint16_t led_current_ma);
uint8_t spectrometerGetAtIME();
uint16_t spectrometerGetAStep();
uint8_t spectrometerGetGain();

bool spectrometerSetAtIMEValue(uint8_t atime_value);
bool spectrometerSetAStepValue(uint16_t astep_value);
bool spectrometerSetGainValue(int gain_value);

bool setCalibrationSlopeValue(float slope_value);
bool setCalibrationInterceptValue(float intercept_value);

void spectrometerPrintNotAvailableError();
void spectrometerPrintUnsupportedDeviceError();
void spectrometerPrintNotYetImplementedError();
void cmd_spectrometer_status();
void cmd_spectrometer_set_atime(int argc, const char *argv[]);
void cmd_spectrometer_set_astep(int argc, const char *argv[]);
void cmd_spectrometer_set_gain(int argc, const char *argv[]);
bool cmd_get_par_raw(float *out_par);
bool cmd_get_par(float *out_par);
bool set_calibration_slope(int argc, const char *argv[]);
bool set_calibration_intercept(int argc, const char *argv[]);
void cmd_set_dev_name(int ar, const char *argv[]);
char *cmd_get_dev_name();
void loadpref();




