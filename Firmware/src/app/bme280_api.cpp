#include <Wire.h>
#include <Preferences.h>
#include "app/bme280_api.h"

namespace {

constexpr uint8_t kBme280AddressPrimary   = 0x76;
constexpr uint8_t kBme280AddressSecondary = 0x77;
constexpr uint8_t kBme280ChipId           = 0x60;

constexpr uint8_t kBme280StatusRegister = 0xF3;
constexpr uint8_t kBme280MeasuringBit   = 0x08;
constexpr uint32_t kBme280MeasurementTimeoutMs = 2000; // generous safety margin over the ~40ms worst case

constexpr const char *kBme280PrefsNamespace = "bme280_cal";
constexpr const char *kBme280PrefsTempCompKey = "temp_comp";

Preferences bme280_preferences;

Adafruit_BME280::sensor_sampling toLibSampling(Bme280Oversampling value) {
  switch (value) {
  case Bme280Oversampling::Off: return Adafruit_BME280::SAMPLING_NONE;
  case Bme280Oversampling::X1:  return Adafruit_BME280::SAMPLING_X1;
  case Bme280Oversampling::X2:  return Adafruit_BME280::SAMPLING_X2;
  case Bme280Oversampling::X4:  return Adafruit_BME280::SAMPLING_X4;
  case Bme280Oversampling::X8:  return Adafruit_BME280::SAMPLING_X8;
  case Bme280Oversampling::X16: return Adafruit_BME280::SAMPLING_X16;
  }
  return Adafruit_BME280::SAMPLING_NONE;
}

Adafruit_BME280::sensor_filter toLibFilter(Bme280Filter value) {
  switch (value) {
  case Bme280Filter::Off: return Adafruit_BME280::FILTER_OFF;
  case Bme280Filter::X2:  return Adafruit_BME280::FILTER_X2;
  case Bme280Filter::X4:  return Adafruit_BME280::FILTER_X4;
  case Bme280Filter::X8:  return Adafruit_BME280::FILTER_X8;
  case Bme280Filter::X16: return Adafruit_BME280::FILTER_X16;
  }
  return Adafruit_BME280::FILTER_OFF;
}

} // namespace

bool Bme280Sensor::begin() {
  available_ = false;
  for (uint8_t addr : {kBme280AddressPrimary, kBme280AddressSecondary}) {
    if (bme_.begin(addr, &Wire) && bme_.sensorID() == kBme280ChipId) {
      address_ = addr;
      available_ = true;
      break;
    }
  }
  if (!available_) return false;

  continuous_mode_enabled_ = false;
  continuous_active_ = false;
  loadTemperatureCompensation();
  applySampling(Adafruit_BME280::MODE_SLEEP);
  return true;
}

void Bme280Sensor::loadTemperatureCompensation() {
  bme280_preferences.begin(kBme280PrefsNamespace, true);
  temp_compensation_c_ = bme280_preferences.getFloat(kBme280PrefsTempCompKey, 0.0f);
  bme280_preferences.end();
  bme_.setTemperatureCompensation(temp_compensation_c_);
}

bool Bme280Sensor::readStatusRegister(uint8_t *status) {
  Wire.beginTransmission(address_);
  Wire.write(kBme280StatusRegister);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<int>(address_), 1) != 1) return false;
  *status = Wire.read();
  return true;
}

bool Bme280Sensor::waitForMeasurementComplete(uint32_t timeout_ms) {
  const uint32_t started = millis();
  uint8_t status = 0;
  while (readStatusRegister(&status) && (status & kBme280MeasuringBit)) {
    if (millis() - started >= timeout_ms) return false;
    delay(2);
  }
  return true;
}

void Bme280Sensor::applySampling(Adafruit_BME280::sensor_mode mode) {
  bme_.setSampling(mode,
                    toLibSampling(temp_osrs_), // always sampled — see header comment
                    press_enabled_ ? toLibSampling(press_osrs_) : Adafruit_BME280::SAMPLING_NONE,
                    hum_enabled_ ? toLibSampling(hum_osrs_) : Adafruit_BME280::SAMPLING_NONE,
                    toLibFilter(filter_),
                    Adafruit_BME280::STANDBY_MS_0_5);
}

void Bme280Sensor::reconfigureIfActive() {
  if (available_ && continuous_mode_enabled_ && continuous_active_) {
    applySampling(Adafruit_BME280::MODE_NORMAL);
  }
}

bool Bme280Sensor::setContinuousModeEnabled(bool enabled) {
  if (!available_) return false;
  continuous_mode_enabled_ = enabled;
  if (!enabled) {
    // Drop back to sleep; a later re-enable will wait for a fresh cycle again.
    continuous_active_ = false;
    applySampling(Adafruit_BME280::MODE_SLEEP);
  }
  return true;
}

bool Bme280Sensor::setTemperatureCompensation(float offset_c) {
  if (!available_) return false;
  temp_compensation_c_ = offset_c;
  bme_.setTemperatureCompensation(offset_c);
  bme280_preferences.begin(kBme280PrefsNamespace, false);
  bme280_preferences.putFloat(kBme280PrefsTempCompKey, offset_c);
  bme280_preferences.end();
  return true;
}

bool Bme280Sensor::setTemperatureOversampling(Bme280Oversampling value) {
  if (!available_) return false;
  // Off would zero out t_fine and silently corrupt pressure/humidity readings
  // (see applySampling's comment) — clamp to the minimum useful setting instead.
  temp_osrs_ = (value == Bme280Oversampling::Off) ? Bme280Oversampling::X1 : value;
  reconfigureIfActive();
  return true;
}

bool Bme280Sensor::setPressureOversampling(Bme280Oversampling value) {
  if (!available_) return false;
  press_osrs_ = value;
  reconfigureIfActive();
  return true;
}

bool Bme280Sensor::setHumidityOversampling(Bme280Oversampling value) {
  if (!available_) return false;
  hum_osrs_ = value;
  reconfigureIfActive();
  return true;
}

bool Bme280Sensor::setIIRFilter(Bme280Filter value) {
  if (!available_) return false;
  filter_ = value;
  reconfigureIfActive();
  return true;
}

void Bme280Sensor::populateReading(Bme280Reading *out) {
  *out = Bme280Reading();
  if (temp_enabled_) {
    out->temperature_c = bme_.readTemperature();
    out->temperature_valid = !isnan(out->temperature_c);
  }
  if (press_enabled_) {
    out->pressure_hpa = bme_.readPressure() / 100.0f;
    out->pressure_valid = !isnan(out->pressure_hpa);
  }
  if (hum_enabled_) {
    out->humidity_pct = bme_.readHumidity();
    out->humidity_valid = !isnan(out->humidity_pct);
  }
}

bool Bme280Sensor::read(Bme280Reading *out) {
  if (!available_ || !out) return false;

  if (continuous_mode_enabled_) {
    if (!continuous_active_) {
      applySampling(Adafruit_BME280::MODE_NORMAL);
      continuous_active_ = true;
      waitForMeasurementComplete(kBme280MeasurementTimeoutMs); // only the first call waits
    }
    populateReading(out);
    return true;
  }

  applySampling(Adafruit_BME280::MODE_FORCED); // writing FORCED mode also triggers the measurement
  const bool completed = waitForMeasurementComplete(kBme280MeasurementTimeoutMs);
  populateReading(out);
  return completed;
}

Bme280Sensor bme280;

// ---------------------------------------------------------------------------
// Raw-mode helpers (no printing) — used by commands.cpp's plain-text protocol.
// ---------------------------------------------------------------------------

bool bme280ReadRaw(Bme280Reading *out) { return bme280.read(out); }

bool bme280SetTempEnableRaw(bool enabled) {
  if (!bme280.available()) return false;
  bme280.setTemperatureEnabled(enabled);
  return true;
}

bool bme280SetPressEnableRaw(bool enabled) {
  if (!bme280.available()) return false;
  bme280.setPressureEnabled(enabled);
  return true;
}

bool bme280SetHumEnableRaw(bool enabled) {
  if (!bme280.available()) return false;
  bme280.setHumidityEnabled(enabled);
  return true;
}

bool bme280SetTempOversampleRaw(int value) {
  if (value < 0 || value > 5) return false;
  return bme280.setTemperatureOversampling(static_cast<Bme280Oversampling>(value));
}

bool bme280SetPressOversampleRaw(int value) {
  if (value < 0 || value > 5) return false;
  return bme280.setPressureOversampling(static_cast<Bme280Oversampling>(value));
}

bool bme280SetHumOversampleRaw(int value) {
  if (value < 0 || value > 5) return false;
  return bme280.setHumidityOversampling(static_cast<Bme280Oversampling>(value));
}

bool bme280SetIIRRaw(int value) {
  if (value < 0 || value > 4) return false;
  return bme280.setIIRFilter(static_cast<Bme280Filter>(value));
}

bool bme280SetContinuousRaw(bool enabled) {
  return bme280.setContinuousModeEnabled(enabled);
}

bool bme280SetTempCompRaw(float offset_c) {
  return bme280.setTemperatureCompensation(offset_c);
}

// ---------------------------------------------------------------------------
// Command-layer entry points — JSON output
// ---------------------------------------------------------------------------

bool initBme280() {
  const bool found = bme280.begin();
  Serial.println(found ? F("BME280 detected") : F("No BME280 detected"));
  return found;
}

void bme280PrintNotAvailableError() {
  Serial.print(F("{\"bme280\":{\"error\":\"not_available\"}}"));
}

void cmd_bme280_status() {
  Serial.print(F("{\"bme280_status\":{\"available\":"));
  Serial.print(bme280.available() ? F("true") : F("false"));
  if (bme280.available()) {
    Serial.print(F(",\"continuous_mode\":"));
    Serial.print(bme280.continuousModeEnabled() ? F("true") : F("false"));
    Serial.print(F(",\"continuous_active\":"));
    Serial.print(bme280.continuousModeActive() ? F("true") : F("false"));
    Serial.print(F(",\"temperature_compensation_c\":"));
    Serial.print(bme280.temperatureCompensation(), 2);
    Serial.print(F(",\"temperature\":{\"enabled\":"));
    Serial.print(bme280.temperatureEnabled() ? F("true") : F("false"));
    Serial.print(F(",\"oversampling\":"));
    Serial.print(static_cast<int>(bme280.temperatureOversampling()));
    Serial.print(F("},\"pressure\":{\"enabled\":"));
    Serial.print(bme280.pressureEnabled() ? F("true") : F("false"));
    Serial.print(F(",\"oversampling\":"));
    Serial.print(static_cast<int>(bme280.pressureOversampling()));
    Serial.print(F("},\"humidity\":{\"enabled\":"));
    Serial.print(bme280.humidityEnabled() ? F("true") : F("false"));
    Serial.print(F(",\"oversampling\":"));
    Serial.print(static_cast<int>(bme280.humidityOversampling()));
    Serial.print(F("},\"iir_filter\":"));
    Serial.print(static_cast<int>(bme280.iirFilter()));
  }
  Serial.print(F("}}"));
}

void cmd_bme280_set_temp_enable(int argc, const char *argv[]) {
  if (!bme280.available()) { bme280PrintNotAvailableError(); return; }
  if (argc < 1) { Serial.print(F("{\"bme280\":{\"error\":\"missing_arg\"}}")); return; }
  bme280.setTemperatureEnabled(atoi(argv[0]) != 0);
  Serial.print(F("{\"bme280\":{\"temperature_enabled\":"));
  Serial.print(bme280.temperatureEnabled() ? F("true") : F("false"));
  Serial.print(F("}}"));
}

void cmd_bme280_set_press_enable(int argc, const char *argv[]) {
  if (!bme280.available()) { bme280PrintNotAvailableError(); return; }
  if (argc < 1) { Serial.print(F("{\"bme280\":{\"error\":\"missing_arg\"}}")); return; }
  bme280.setPressureEnabled(atoi(argv[0]) != 0);
  Serial.print(F("{\"bme280\":{\"pressure_enabled\":"));
  Serial.print(bme280.pressureEnabled() ? F("true") : F("false"));
  Serial.print(F("}}"));
}

void cmd_bme280_set_hum_enable(int argc, const char *argv[]) {
  if (!bme280.available()) { bme280PrintNotAvailableError(); return; }
  if (argc < 1) { Serial.print(F("{\"bme280\":{\"error\":\"missing_arg\"}}")); return; }
  bme280.setHumidityEnabled(atoi(argv[0]) != 0);
  Serial.print(F("{\"bme280\":{\"humidity_enabled\":"));
  Serial.print(bme280.humidityEnabled() ? F("true") : F("false"));
  Serial.print(F("}}"));
}

void cmd_bme280_set_temp_oversample(int argc, const char *argv[]) {
  if (!bme280.available()) { bme280PrintNotAvailableError(); return; }
  if (argc < 1) { Serial.print(F("{\"bme280\":{\"error\":\"missing_arg\"}}")); return; }
  if (!bme280SetTempOversampleRaw(atoi(argv[0]))) {
    Serial.print(F("{\"bme280\":{\"error\":\"oversampling_out_of_range_0_5\"}}"));
    return;
  }
  Serial.print(F("{\"bme280\":{\"temperature_oversampling\":"));
  Serial.print(static_cast<int>(bme280.temperatureOversampling()));
  Serial.print(F("}}"));
}

void cmd_bme280_set_press_oversample(int argc, const char *argv[]) {
  if (!bme280.available()) { bme280PrintNotAvailableError(); return; }
  if (argc < 1) { Serial.print(F("{\"bme280\":{\"error\":\"missing_arg\"}}")); return; }
  if (!bme280SetPressOversampleRaw(atoi(argv[0]))) {
    Serial.print(F("{\"bme280\":{\"error\":\"oversampling_out_of_range_0_5\"}}"));
    return;
  }
  Serial.print(F("{\"bme280\":{\"pressure_oversampling\":"));
  Serial.print(static_cast<int>(bme280.pressureOversampling()));
  Serial.print(F("}}"));
}

void cmd_bme280_set_hum_oversample(int argc, const char *argv[]) {
  if (!bme280.available()) { bme280PrintNotAvailableError(); return; }
  if (argc < 1) { Serial.print(F("{\"bme280\":{\"error\":\"missing_arg\"}}")); return; }
  if (!bme280SetHumOversampleRaw(atoi(argv[0]))) {
    Serial.print(F("{\"bme280\":{\"error\":\"oversampling_out_of_range_0_5\"}}"));
    return;
  }
  Serial.print(F("{\"bme280\":{\"humidity_oversampling\":"));
  Serial.print(static_cast<int>(bme280.humidityOversampling()));
  Serial.print(F("}}"));
}

void cmd_bme280_set_iir(int argc, const char *argv[]) {
  if (!bme280.available()) { bme280PrintNotAvailableError(); return; }
  if (argc < 1) { Serial.print(F("{\"bme280\":{\"error\":\"missing_arg\"}}")); return; }
  if (!bme280SetIIRRaw(atoi(argv[0]))) {
    Serial.print(F("{\"bme280\":{\"error\":\"iir_out_of_range_0_4\"}}"));
    return;
  }
  Serial.print(F("{\"bme280\":{\"iir_filter\":"));
  Serial.print(static_cast<int>(bme280.iirFilter()));
  Serial.print(F("}}"));
}

namespace {

void printReadingJson(const Bme280Reading &r) {
  Serial.print(F("{\"bme280\":{\"temperature_c\":"));
  if (r.temperature_valid) Serial.print(r.temperature_c, 2); else Serial.print(F("null"));
  Serial.print(F(",\"pressure_hpa\":"));
  if (r.pressure_valid) Serial.print(r.pressure_hpa, 2); else Serial.print(F("null"));
  Serial.print(F(",\"humidity_pct\":"));
  if (r.humidity_valid) Serial.print(r.humidity_pct, 2); else Serial.print(F("null"));
  Serial.print(F("}}"));
}

} // namespace

void cmd_bme280_get_tph() {
  if (!bme280.available()) { bme280PrintNotAvailableError(); return; }
  Bme280Reading reading;
  if (!bme280ReadRaw(&reading)) {
    Serial.print(F("{\"bme280\":{\"error\":\"read_failed\"}}"));
    return;
  }
  printReadingJson(reading);
}

void cmd_bme280_set_continuous(int argc, const char *argv[]) {
  if (!bme280.available()) { bme280PrintNotAvailableError(); return; }
  if (argc < 1) { Serial.print(F("{\"bme280\":{\"error\":\"missing_arg\"}}")); return; }
  bme280SetContinuousRaw(atoi(argv[0]) != 0);
  Serial.print(F("{\"bme280\":{\"continuous_mode\":"));
  Serial.print(bme280.continuousModeEnabled() ? F("true") : F("false"));
  Serial.print(F("}}"));
}

void cmd_bme280_set_temp_comp(int argc, const char *argv[]) {
  if (!bme280.available()) { bme280PrintNotAvailableError(); return; }
  if (argc < 1) { Serial.print(F("{\"bme280\":{\"error\":\"missing_arg\"}}")); return; }
  bme280SetTempCompRaw(atof(argv[0]));
  Serial.print(F("{\"bme280\":{\"temperature_compensation_c\":"));
  Serial.print(bme280.temperatureCompensation(), 2);
  Serial.print(F("}}"));
}

void cmd_bme280_get_temp_comp() {
  if (!bme280.available()) { bme280PrintNotAvailableError(); return; }
  Serial.print(F("{\"bme280\":{\"temperature_compensation_c\":"));
  Serial.print(bme280.temperatureCompensation(), 2);
  Serial.print(F("}}"));
}
