#pragma once

#include <Arduino.h>
#include <Adafruit_BME280.h>

// Oversampling levels, shared by the temperature/pressure/humidity setters.
// Values match the ordinal expected on the wire protocol (bme_set_*_oversample,<0-5>).
enum class Bme280Oversampling : uint8_t {
  Off = 0,
  X1  = 1,
  X2  = 2,
  X4  = 3,
  X8  = 4,
  X16 = 5,
};

// IIR filter coefficient (bme_set_iir,<0-4>).
enum class Bme280Filter : uint8_t {
  Off = 0,
  X2  = 1,
  X4  = 2,
  X8  = 3,
  X16 = 4,
};

struct Bme280Reading {
  bool  temperature_valid = false;
  float temperature_c     = 0.0f;
  bool  pressure_valid    = false;
  float pressure_hpa      = 0.0f;
  bool  humidity_valid    = false;
  float humidity_pct      = 0.0f;
};

// Thin wrapper around Adafruit_BME280 that adds: optional-chip detection,
// per-channel enable flags, per-channel oversampling, IIR filtering, and a
// forced-vs-continuous read mode matching the device's command set.
class Bme280Sensor {
public:
  // Probes both possible I2C addresses (0x76, 0x77) and initializes the chip
  // if found. Safe to call when no BME280 is fitted — returns false.
  bool begin();
  bool available() const { return available_; }

  // false (default) = one-shot: every read() takes its own forced-mode
  // measurement. true = continuous: the chip free-runs in normal mode.
  bool setContinuousModeEnabled(bool enabled);
  bool continuousModeEnabled() const { return continuous_mode_enabled_; }
  // Only meaningful in continuous mode: whether the chip has actually been
  // switched into normal mode yet (false until the first read() call).
  bool continuousModeActive() const { return continuous_active_; }

  void setTemperatureEnabled(bool enabled) { temp_enabled_ = enabled; }
  void setPressureEnabled(bool enabled)    { press_enabled_ = enabled; reconfigureIfActive(); }
  void setHumidityEnabled(bool enabled)    { hum_enabled_ = enabled; reconfigureIfActive(); }
  bool temperatureEnabled() const { return temp_enabled_; }
  bool pressureEnabled() const    { return press_enabled_; }
  bool humidityEnabled() const    { return hum_enabled_; }

  bool setTemperatureOversampling(Bme280Oversampling value);
  bool setPressureOversampling(Bme280Oversampling value);
  bool setHumidityOversampling(Bme280Oversampling value);
  Bme280Oversampling temperatureOversampling() const { return temp_osrs_; }
  Bme280Oversampling pressureOversampling() const    { return press_osrs_; }
  Bme280Oversampling humidityOversampling() const    { return hum_osrs_; }

  bool setIIRFilter(Bme280Filter value);
  Bme280Filter iirFilter() const { return filter_; }

  // Added to every raw temperature reading (and folded into the t_fine used
  // by the pressure/humidity compensation formulas) to null out self-heating
  // bias. Persists across reboots.
  bool setTemperatureCompensation(float offset_c);
  float temperatureCompensation() const { return temp_compensation_c_; }

  // bme_get_tph: takes a forced-mode reading in one-shot mode (the default), or in
  // continuous mode switches the chip into normal mode on the first call
  // (blocking for that first conversion) and just samples the free-running
  // registers on every call after that.
  bool read(Bme280Reading *out);

private:
  // Temperature is always sampled at temp_osrs_ regardless of temp_enabled_:
  // the BME280 compensation formulas for pressure and humidity both depend on
  // t_fine, so skipping the temperature ADC would corrupt P/H readings even
  // when temperature itself isn't being reported.
  void applySampling(Adafruit_BME280::sensor_mode mode);
  void reconfigureIfActive();
  bool waitForMeasurementComplete(uint32_t timeout_ms);
  bool readStatusRegister(uint8_t *status);
  void populateReading(Bme280Reading *out);
  void loadTemperatureCompensation();

  Adafruit_BME280 bme_;
  bool available_ = false;
  uint8_t address_ = 0;

  bool temp_enabled_  = true;
  bool press_enabled_ = true;
  bool hum_enabled_   = true;

  Bme280Oversampling temp_osrs_  = Bme280Oversampling::X16;
  Bme280Oversampling press_osrs_ = Bme280Oversampling::X16;
  Bme280Oversampling hum_osrs_   = Bme280Oversampling::X16;
  Bme280Filter       filter_     = Bme280Filter::Off;

  float temp_compensation_c_ = 0.0f;

  bool continuous_mode_enabled_ = false;
  bool continuous_active_       = false;
};

extern Bme280Sensor bme280;

// ---------------------------------------------------------------------------
// Command-layer entry points — mirrors the spectrometer facade in
// spectrometer_api.h: cmd_* functions print JSON directly and are called from
// commands.cpp's jsonMode branch, while the plain accessors below let
// commands.cpp build the raw-mode (non-JSON) text output.
// ---------------------------------------------------------------------------
bool initBme280();
void bme280PrintNotAvailableError();

void cmd_bme280_status();
void cmd_bme280_set_temp_enable(int argc, const char *argv[]);
void cmd_bme280_set_press_enable(int argc, const char *argv[]);
void cmd_bme280_set_hum_enable(int argc, const char *argv[]);
void cmd_bme280_set_temp_oversample(int argc, const char *argv[]);
void cmd_bme280_set_press_oversample(int argc, const char *argv[]);
void cmd_bme280_set_hum_oversample(int argc, const char *argv[]);
void cmd_bme280_set_iir(int argc, const char *argv[]);
void cmd_bme280_set_continuous(int argc, const char *argv[]);
void cmd_bme280_set_temp_comp(int argc, const char *argv[]);
void cmd_bme280_get_temp_comp();
void cmd_bme280_get_tph();

// Raw-mode helpers (no printing) for commands.cpp's plain-text protocol.
bool bme280ReadRaw(Bme280Reading *out);
bool bme280SetTempEnableRaw(bool enabled);
bool bme280SetPressEnableRaw(bool enabled);
bool bme280SetHumEnableRaw(bool enabled);
bool bme280SetTempOversampleRaw(int value);
bool bme280SetPressOversampleRaw(int value);
bool bme280SetHumOversampleRaw(int value);
bool bme280SetIIRRaw(int value);
bool bme280SetContinuousRaw(bool enabled);
bool bme280SetTempCompRaw(float offset_c);
