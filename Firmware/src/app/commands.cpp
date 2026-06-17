#include <Wire.h>
#include "app/commands.h"
#include "HWCDC.h"
#include "app/debug_api.h"
#include "app/spectrometer_api.h"
#include "app/device_config.h"
#include <esp_system.h>

#include <ArduinoJson.h>


// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void printJsonSafeString(const char *s) {
  while (*s) {
    char c = *s++;
    if (c == '"' || c == '\\') {
      Serial.print('\\');
      Serial.print(c);
    } else if (c < 0x20) {
      // skip control characters
    } else {
      Serial.print(c);
    }
  }
}

static void printRawI2CScan() {
  bool first = true;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      if (!first)
        Serial.print(',');
      first = false;
      Serial.print(F("0x"));
      if (addr < 0x10)
        Serial.print('0');
      Serial.print(addr, HEX);
    }
  }
}

static bool printRawSpectrometerChannels(const SpectrometerResult &result, bool corrected) {
  if (result.channel_count == 0) {
    Serial.print(F("error:no_channels"));
    return false;
  }

  Serial.print(spectrometerModelName(result.model));
  Serial.print(',');
  for (uint8_t i = 0; i < result.channel_count; i++) {
    if (i > 0)
      Serial.print(',');
    if (corrected) {
      float correctedVal = (float)result.channels[i] * par_coefficients[i];
      Serial.print(correctedVal, 4);
    } else {
      Serial.print(result.channels[i]);
    }
  }
  return true;
}

static bool printRawSpectrometerRead() {
  if (!spectrometerPrepareLegacyCommand())
    return false;

  SpectrometerResult result;
  if (!spectrometerReadInto(&result)) {
    Serial.print(F("error:read_failed"));
    return false;
  }
  return printRawSpectrometerChannels(result, true);
}

static bool printRawSpectrometerReadRaw() {
  if (!spectrometerPrepareLegacyCommand())
    return false;

  SpectrometerResult result;
  if (!spectrometerReadInto(&result)) {
    Serial.print(F("error:read_failed"));
    return false;
  }
  return printRawSpectrometerChannels(result, false);
}

static bool printRawSpectrometerReadFlash(uint16_t led_current_ma) {
  if (!spectrometerPrepareLegacyCommand())
    return false;

  if (led_current_ma > 20)
    led_current_ma = 20;

  SpectrometerResult dark, lit, diff;
  if (!spectrometerReadInto(&dark)) {
    Serial.print(F("error:dark_read_failed"));
    return false;
  }

  const uint16_t actual_led_ma = spectrometerSetLedCurrentSilent(led_current_ma);
  if (actual_led_ma == 0xFFFF) {
    Serial.print(F("error:led_set_failed"));
    return false;
  }

  const uint8_t atime = spectrometerGetAtIME();
  const uint16_t astep = spectrometerGetAStep();
  uint32_t t_ms = ((uint32_t)(atime + 1) * (uint32_t)(astep + 1) * 278ul + 99999ul) / 100000ul + 2ul;
  if (t_ms < 5u) t_ms = 5u;
  delay(t_ms);

  if (!spectrometerReadInto(&lit)) {
    spectrometerSetLedCurrentSilent(0);
    Serial.print(F("error:lit_read_failed"));
    return false;
  }

  spectrometerSetLedCurrentSilent(0);

  diff.model = dark.model;
  diff.channel_count = dark.channel_count;
  diff.sat_mask = dark.sat_mask | lit.sat_mask;
  for (uint8_t i = 0; i < dark.channel_count; i++) {
    diff.channels[i] = lit.channels[i] > dark.channels[i] ? lit.channels[i] - dark.channels[i] : 0;
  }

  Serial.print(F("dark:"));
  printRawSpectrometerChannels(dark, false);
  Serial.print(F(";lit:"));
  printRawSpectrometerChannels(lit, false);
  Serial.print(F(";diff:"));
  printRawSpectrometerChannels(diff, false);
  return true;
}

static bool printRawSpectrometerSetLedCurrent(uint16_t led_current_ma) {
  if (!spectrometerPrepareLegacyCommand())
    return false;

  const uint16_t actual_ma = spectrometerSetLedCurrentSilent(led_current_ma);
  if (actual_ma == 0xFFFF) {
    Serial.print(F("error:led_set_failed"));
    return false;
  }
  Serial.print(actual_ma);
  return true;
}

static bool printRawSpectrometerSetAtime(const char *arg) {
  if (!arg || *arg == '\0') {
    Serial.print(F("error:missing_arg"));
    return false;
  }
  int val = atoi(arg);
  if (val < 0 || val > 255) {
    Serial.print(F("error:atime_out_of_range"));
    return false;
  }
  if (!spectrometerSetAtIMEValue(static_cast<uint8_t>(val))) {
    Serial.print(F("error:set_failed"));
    return false;
  }
  Serial.print(val);
  return true;
}

static bool printRawSpectrometerSetAStep(const char *arg) {
  if (!arg || *arg == '\0') {
    Serial.print(F("error:missing_arg"));
    return false;
  }
  long val = atol(arg);
  if (val < 0 || val > 65534) {
    Serial.print(F("error:astep_out_of_range"));
    return false;
  }
  if (!spectrometerSetAStepValue(static_cast<uint16_t>(val))) {
    Serial.print(F("error:set_failed"));
    return false;
  }
  Serial.print(val);
  return true;
}

static bool printRawSpectrometerSetGain(const char *arg) {
  if (!arg || *arg == '\0') {
    Serial.print(F("error:missing_arg"));
    return false;
  }
  int val = atoi(arg);
  const int max_gain = (spectrometer_model == SpectrometerModel::AS7341) ? 10 : 12;
  if (val < 0 || val > max_gain) {
    Serial.print(F("error:gain_out_of_range"));
    return false;
  }
  if (!spectrometerSetGainValue(val)) {
    Serial.print(F("error:set_failed"));
    return false;
  }
  Serial.print(val);
  return true;
}

static void printRawSpectrometerStatus() {
  Serial.print(F("model="));
  Serial.print(spectrometerModelName(spectrometer_model));
  Serial.print(F(",available="));
  Serial.print(spectrometer_available ? 1 : 0);
  if (spectrometer_available) {
    Serial.print(F(",atime="));
    Serial.print(spectrometerGetAtIME());
    Serial.print(F(",astep="));
    Serial.print(spectrometerGetAStep());
    Serial.print(F(",gain="));
    Serial.print(spectrometerGetGain());
  }
}

static bool setCalibrationSlopeRaw(const char *arg) {
  if (!arg || *arg == '\0') {
    Serial.print(F("error:missing_args"));
    return false;
  }
  float value = atof(arg);
  if (!setCalibrationSlopeValue(value)) {
    Serial.print(F("error:set_failed"));
    return false;
  }
  Serial.print(value);
  return true;
}

static bool setCalibrationInterceptRaw(const char *arg) {
  if (!arg || *arg == '\0') {
    Serial.print(F("error:missing_args"));
    return false;
  }
  float value = atof(arg);
  if (!setCalibrationInterceptValue(value)) {
    Serial.print(F("error:set_failed"));
    return false;
  }
  Serial.print(value);
  return true;
}

static void printRawGetDeviceName() {
  Serial.print(cmd_get_dev_name());
}

// ---------------------------------------------------------------------------
// JSON protocol wrapper
// ---------------------------------------------------------------------------

static bool serial_string_init() {
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  Serial.print("{\"device_name\":\"" DEVICE_PRODUCT_NAME "\",\"device_version\":\"" DEVICE_VERSION "\",\"device_id\":\"");
  Serial.print(mac_str);
  Serial.print("\",\"device_battery\":\"NaN\",\"device_firmware\":" DEVICE_FIRMWARE_VERSION_STR ",\"sample\":[{\"protocol_id\":\"NaN\",\"set\":[");
  return true;
}

static bool serial_string_end() {
  Serial.print(F("]}]}" DEVICE_FRAME_FOOTER));
  Serial.print('\n');
  return true;
}

bool handleJsonProtocol(const String &json) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);

  if (err) {
    Serial.print(F("{\"error\":\"json_parse_error\",\"detail\":\""));
    Serial.print(err.c_str());
    Serial.println(F("\"}"));
    return false;
  }

  bool handledAny = false;
  bool firstOut = true;

  serial_string_init();

  auto processProtocolSet = [&](JsonArray proto) {
    for (JsonVariant setV : proto) {
      JsonObject setObj = setV.as<JsonObject>();
      if (setObj.isNull())
        continue;

      const char *cmd = setObj["label"];
      if (!cmd)
        continue;

      uint16_t repeats = setObj["protocol_repeats"] | 1;
      if (repeats == 0)
        repeats = 1;

      for (uint16_t i = 0; i < repeats; i++) {
        if (!firstOut)
          Serial.print(',');
        firstOut = false;

        handleCommandText(String(cmd), true);
        handledAny = true;
      }
    }
  };

  if (doc.is<JsonArray>()) {
    JsonArray arr = doc.as<JsonArray>();
    // Try: array of objects each containing a "set" key
    for (JsonVariant v : arr) {
      JsonObject obj = v.as<JsonObject>();
      if (obj.isNull())
        continue;
      JsonArray proto = obj["set"].as<JsonArray>();
      if (!proto.isNull())
        processProtocolSet(proto);
    }
    // Fallback: treat the array itself as the command list
    if (!handledAny)
      processProtocolSet(arr);
  } else if (doc.is<JsonObject>()) {
    // Iterate all members; process any array value as the command list
    for (JsonPair kv : doc.as<JsonObject>()) {
      JsonArray proto = kv.value().as<JsonArray>();
      if (!proto.isNull()) {
        processProtocolSet(proto);
        break;
      }
    }
  } else {
    serial_string_end();
    return false;
  }

  serial_string_end();
  return handledAny;
}





bool handleCommandText(const String &cmd, bool jsonMode) {
  if (cmd == "hello") {
    if (jsonMode) {
      Serial.print(F("{\"device\":\"" DEVICE_PRODUCT_NAME "\",\"version\":\"" DEVICE_VERSION "\"}"));
    } else {
      Serial.print(F(DEVICE_PRODUCT_NAME "," DEVICE_VERSION "," DEVICE_FIRMWARE_VERSION_STR));
    }

  } else if (cmd == "battery") {
    if (jsonMode) {
      Serial.print(F("{\"battery\":\"NaN\"}"));
    } else {
      Serial.print(F("NaN"));
    }

  } else if (cmd == "i2c_scan") {
    if (jsonMode) {
      i2c_scan();
    } else {
      printRawI2CScan();
    }

  } else if (cmd == "spec") {
    if (jsonMode) {
      spectrometer_read();
    } else {
      printRawSpectrometerRead();
    }

  } else if (cmd == "spec_raw") {
    if (jsonMode) {
      spectrometer_read_raw();
    } else {
      printRawSpectrometerReadRaw();
    }

  } else if (cmd.startsWith("set_led")) {
    int ledCurrent = 10; // default LED current in mA
    int comma = cmd.indexOf(',');
    if (comma > 0) {
      String arg = cmd.substring(comma + 1);
      arg.trim();
      ledCurrent = arg.toInt();
    }
    if (jsonMode) {
      spectrometer_set_led_current(static_cast<uint16_t>(ledCurrent));
    } else {
      printRawSpectrometerSetLedCurrent(static_cast<uint16_t>(ledCurrent));
    }

  } else if (cmd.startsWith("spec_flash")) {
    int ledCurrent = 10; // default LED current in mA
    int comma = cmd.indexOf(',');
    if (comma > 0) {
      String arg = cmd.substring(comma + 1);
      arg.trim();
      ledCurrent = arg.toInt();
    }
    if (jsonMode) {
      spectrometer_read_flash(static_cast<uint16_t>(ledCurrent));
    } else {
      printRawSpectrometerReadFlash(static_cast<uint16_t>(ledCurrent));
    }

  } else if (cmd.startsWith("spec_set_atime")) {
    int comma = cmd.indexOf(',');
    const char *arg = (comma > 0) ? cmd.c_str() + comma + 1 : "";
    if (jsonMode) {
      cmd_spectrometer_set_atime(comma > 0 ? 1 : 0, &arg);
    } else {
      printRawSpectrometerSetAtime(arg);
    }

  } else if (cmd.startsWith("spec_set_astep")) {
    int comma = cmd.indexOf(',');
    const char *arg = (comma > 0) ? cmd.c_str() + comma + 1 : "";
    if (jsonMode) {
      cmd_spectrometer_set_astep(comma > 0 ? 1 : 0, &arg);
    } else {
      printRawSpectrometerSetAStep(arg);
    }

  } else if (cmd.startsWith("spec_set_gain")) {
    int comma = cmd.indexOf(',');
    const char *arg = (comma > 0) ? cmd.c_str() + comma + 1 : "";
    if (jsonMode) {
      cmd_spectrometer_set_gain(comma > 0 ? 1 : 0, &arg);
    } else {
      printRawSpectrometerSetGain(arg);
    }

  } else if (cmd == "spec_status" || cmd == "status") {
    if (jsonMode) {
      cmd_spectrometer_status();
    } else {
      printRawSpectrometerStatus();
    }

  } else if (cmd == "par_raw") {
    float par_raw = 0;
    if (cmd_get_par_raw(&par_raw)) {
      if (jsonMode) {
        Serial.print(F("{\"par_raw\":"));
        Serial.print(par_raw);
        Serial.print(F("}"));
      } else {
        Serial.print(par_raw);
      }
    }

  } else if (cmd == "par") {
    float par = 0;
    if (cmd_get_par(&par)) {
      if (jsonMode) {
        Serial.print(F("{\"par\":"));
        Serial.print(par);
        Serial.print(F("}"));
      } else {
        Serial.print(par);
      }
    }

  } else if (cmd.startsWith("cal_par_slope")) {
    int comma = cmd.indexOf(',');
    if (comma > 0) {
      const char *arg = cmd.c_str() + comma + 1;
      if (jsonMode) {
        set_calibration_slope(1, &arg);
      } else {
        setCalibrationSlopeRaw(arg);
      }
    } else {
      if (jsonMode) {
        Serial.print(F("{\"calibration\":{\"error\":\"missing_args\"}}"));
      } else {
        Serial.print(F("error:missing_args"));
      }
    }

  } else if (cmd.startsWith("cal_par_intercept")) {
    int comma = cmd.indexOf(',');
    if (comma > 0) {
      const char *arg = cmd.c_str() + comma + 1;
      if (jsonMode) {
        set_calibration_intercept(1, &arg);
      } else {
        setCalibrationInterceptRaw(arg);
      }
    } else {
      if (jsonMode) {
        Serial.print(F("{\"calibration\":{\"error\":\"missing_args\"}}"));
      } else {
        Serial.print(F("error:missing_args"));
      }
    }

  } else if (cmd.startsWith("set_name")) {
    int comma = cmd.indexOf(',');
    if (comma > 0) {
      const char *arg = cmd.c_str() + comma + 1;
      cmd_set_dev_name(1, &arg);
      if (jsonMode) {
        Serial.print(F("{\"device_name\":\""));
        printJsonSafeString(cmd_get_dev_name());
        Serial.print(F("\"}"));
      } else {
        printRawGetDeviceName();
      }
    } else {
      if (jsonMode) {
        Serial.print(F("{\"error\":\"missing_args\"}"));
      } else {
        Serial.print(F("error:missing_args"));
      }
    }

  } else if (cmd.startsWith("get_name")) {
    if (jsonMode) {
      Serial.print(F("{\"device_name\":\""));
      printJsonSafeString(cmd_get_dev_name());
      Serial.print(F("\"}"));
    } else {
      printRawGetDeviceName();
    }

  } else if (cmd.startsWith("set_spec_coeff")) {
    // Format: set_spec_coeff,<channel>,<value>
    int firstComma = cmd.indexOf(',');
    if (firstComma > 0) {
      const char *remaining = cmd.c_str() + firstComma + 1;
      String remainingStr(remaining);
      int secondComma = remainingStr.indexOf(',');
      if (secondComma > 0) {
        const char *argv[] = {
          remaining,
          remaining + secondComma + 1
        };
        if (jsonMode) {
          cmd_set_spec_coeff(2, argv);
        } else {
          cmd_set_spec_coeff(2, argv);
        }
      } else {
        if (jsonMode) {
          Serial.print(F("{\"spectrometer_coeff\":{\"error\":\"missing_args\"}}"));
        } else {
          Serial.print(F("error:missing_args"));
        }
      }
    } else {
      if (jsonMode) {
        Serial.print(F("{\"spectrometer_coeff\":{\"error\":\"missing_args\"}}"));
      } else {
        Serial.print(F("error:missing_args"));
      }
    }

  } else if (cmd.startsWith("get_spec_coeff")) {
    // Format: get_spec_coeff or get_spec_coeff,<channel>
    int comma = cmd.indexOf(',');
    if (comma > 0) {
      const char *arg = cmd.c_str() + comma + 1;
      const char *argv[] = { arg };
      if (jsonMode) {
        cmd_get_spec_coeff(1, argv);
      } else {
        cmd_get_spec_coeff(1, argv);
      }
    } else {
      // No argument - return all coefficients
      if (jsonMode) {
        cmd_get_spec_coeff(0, nullptr);
      } else {
        // For raw mode, print all coefficients comma-separated
        for (int i = 0; i < 18; i++) {
          if (i > 0) Serial.print(',');
          Serial.print(par_coefficients[i], 6);
        }
      }
    }

  } else if (cmd == "reboot") {
    if (jsonMode) {
      cmd_reboot();
    } else {
      Serial.println(F("reboot"));
      Serial.flush();
      esp_restart();
    }

  } else if (cmd.length() > 0) {
    if (jsonMode) {
      Serial.print(F("{\"error\":\"unknown_command\"}"));
    } else {
      Serial.print(F("error:unknown_command"));
    }
    return false;
  }
  return true;
}
