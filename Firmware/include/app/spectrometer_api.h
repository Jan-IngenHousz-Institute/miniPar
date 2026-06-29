#pragma once

#include <Arduino.h>
#include <stdint.h>
#include "app/spectrometer_types.h"

extern bool spectrometer_available;
extern SpectrometerModel spectrometer_model;

extern float par_coefficients[18]; // per-channel PAR conversion coefficients, indexed 0..channel_count-1

// ---------------------------------------------------------------------------
// Default AS7341 acquisition settings — must match initAS7341() in as7341_api.cpp
// ---------------------------------------------------------------------------
static constexpr uint8_t  kAs7341DefaultAtIME         = 100;
static constexpr uint16_t kAs7341DefaultAStep          = 999;
static constexpr float    kAs7341DefaultGainMultiplier  = 2.0f;   // AS7341_GAIN_2X = 2×
static constexpr float    kAs7341DefaultIntTimeS        =
    (kAs7341DefaultAtIME + 1.0f) * (kAs7341DefaultAStep + 1.0f) * 2.78e-6f;
// Scaling factor: old raw-based coeff × this = basic-count-based coeff at default settings
static constexpr float    kAs7341BasicCountScale        =
    kAs7341DefaultGainMultiplier * kAs7341DefaultIntTimeS;

// Default PAR coefficients for AS7341, pre-scaled for basic_count = raw / gain / int_time.
// Original calibration values multiplied by kAs7341BasicCountScale so that at the default
// gain/integration settings the PAR result is identical to the original calibration.
static constexpr float kDefaultParCoefficients[18] = {
     1.0947163691214081f  * kAs7341BasicCountScale,
     0.09447067722660155f * kAs7341BasicCountScale,
     0.19681138419847008f * kAs7341BasicCountScale,
     0.16474806285828358f * kAs7341BasicCountScale,
     0.1871980220095715f  * kAs7341BasicCountScale,
     0.12997367405923924f * kAs7341BasicCountScale,
     0.16852358846871307f * kAs7341BasicCountScale,
     0.06357869488305948f * kAs7341BasicCountScale,
    -0.10794982525254658f  * kAs7341BasicCountScale,
    -0.003041914118767322f * kAs7341BasicCountScale,
     0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f
};

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
// Returns gain × integration_time for the current settings (divisor for basic counts)
float spectrometerGetBasicCountDivisor();

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
bool cmd_set_spec_coeff(int argc, const char *argv[]);
bool cmd_get_spec_coeff(int argc, const char *argv[]);
void loadpref();




