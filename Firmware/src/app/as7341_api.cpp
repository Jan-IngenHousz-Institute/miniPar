#include <Wire.h>
#include <Adafruit_AS7341.h>
#include "app/as7341_api.h"

namespace {

constexpr uint8_t kAs7341I2cAddress  = 0x39;
constexpr uint8_t kAs7341WhoamiReg   = 0x92;
// Chip ID check: (raw & 0xFC) == (0x09 << 2) == 0x24
constexpr uint8_t kAs7341ChipIdMask    = 0xFC;
constexpr uint8_t kAs7341ChipIdMasked  = 0x24;

// Adafruit channel index -> SpectrometerResult.channels[] index mapping.
// Indices 4 and 5 of the Adafruit array are SMUX pass-1 intermediates and must
// be skipped.  The 10 remaining channels map in order:
//   Adafruit[0..3]  -> channels[0..3]  (f1_415..f4_515)
//   Adafruit[6..11] -> channels[4..9]  (f5_555..nir)
constexpr uint8_t kAdafruitLen = 12;
constexpr uint8_t kResultLen   = 10;

// ---------------------------------------------------------------------------
// Registers touched directly.  The Adafruit library reaches these through
// private helpers, so the two SMUX steps are reproduced here rather than
// forking the library — see as7341_readInto() for why the library's own
// readAllChannels() cannot be used when saturation matters.
// ---------------------------------------------------------------------------
constexpr uint8_t kAs7341Enable   = 0x80;  // PON=bit0, SP_EN=bit1, SMUXEN=bit4
constexpr uint8_t kAs7341Status   = 0x93;  // latched interrupt status, write-1-to-clear
constexpr uint8_t kAs7341Ch0DataL = 0x95;  // CH0_DATA_L .. CH5_DATA_H = 0x95..0xA0
constexpr uint8_t kAs7341Status2  = 0xA3;  // AVALID / ASAT for the integration just finished
constexpr uint8_t kAs7341Cfg6     = 0xAF;  // SMUX command in bits[4:3]
constexpr uint8_t kAs7341Intenab  = 0xF9;  // interrupt enables (read only, for diagnostics)

constexpr uint8_t kAs7341SpEnBit    = 0x02;
constexpr uint8_t kAs7341SmuxEnBit  = 0x10;
constexpr uint8_t kAs7341SmuxCmdWrite = 0x02 << 3;  // CFG6 bits[4:3] = 0b10
constexpr uint8_t kAs7341SmuxCmdMask  = 0x03 << 3;

// STATUS2 bit layout. AVALID at bit 6 is confirmed by the Adafruit library
// (getIsDataReady reads bit 6 of 0xA3). The two ASAT positions are inherited from the
// AS7343 sibling in this repo, where they were hardware-verified (as7343_api.cpp:52-54,
// "verified 0x44=AVALID|ASAT_ANA"); the parts share the AVALID position, which is the
// evidence they share the layout.
// TODO(bringup): confirm on an AS7341 with `spec_diag`, which dumps this byte raw.
constexpr uint8_t kAs7341AvalidBit   = 0x40;  // bit 6
constexpr uint8_t kAs7341AsatAnalog  = 0x04;  // bit 2
constexpr uint8_t kAs7341AsatDigital = 0x08;  // bit 3
constexpr uint8_t kAs7341AsatAny     = kAs7341AsatAnalog | kAs7341AsatDigital;

// Full scale of the ADC counter for the current exposure. AN000633 p.7 footnote 1:
// "TINT directly determines the Full Scale Range and saturation" — it is NOT a fixed
// 0xFFFF. The uint32_t is load-bearing: (255+1)*(65534+1) overflows 16 bits.
uint16_t as7341FullScale(uint8_t atime, uint16_t astep) {
  const uint32_t full_scale = (uint32_t)(atime + 1u) * (uint32_t)(astep + 1u);
  return full_scale >= 0xFFFFu ? 0xFFFFu : (uint16_t)full_scale;
}

// ---------------------------------------------------------------------------
// Low-level I2C helpers, same shape as the proven ones in as7343_api.cpp
// ---------------------------------------------------------------------------
bool writeRegister8(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kAs7341I2cAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readRegister8(uint8_t reg, uint8_t *value) {
  if (!value) return false;
  Wire.beginTransmission(kAs7341I2cAddress);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<int>(kAs7341I2cAddress), 1) != 1) return false;
  *value = Wire.read();
  return true;
}

bool updateRegister8(uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t current = 0;
  if (!readRegister8(reg, &current)) return false;
  const uint8_t updated = (uint8_t)((current & ~mask) | (value & mask));
  return updated == current ? true : writeRegister8(reg, updated);
}

} // namespace

static Adafruit_AS7341 as7341;

bool initAS7341() {
  if (!as7341.begin()) {
    return false;
  }
  as7341.setATIME(100);
  as7341.setASTEP(999);
  as7341.setGain(AS7341_GAIN_2X);
  return true;
}

bool as7341_readAndValidateChipId(uint8_t *raw_out) {
  Wire.beginTransmission(kAs7341I2cAddress);
  Wire.write(kAs7341WhoamiReg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(static_cast<int>(kAs7341I2cAddress), 1) != 1) {
    return false;
  }
  const uint8_t raw = Wire.read();
  if (raw_out) {
    *raw_out = raw;
  }
  return (raw & kAs7341ChipIdMask) == kAs7341ChipIdMasked;
}

// Register bytes captured during the most recent as7341_readInto(), kept for `spec_diag`.
static uint8_t s_last_status2_low  = 0;  // STATUS2 after the F1-F4 integration
static uint8_t s_last_status2_high = 0;  // STATUS2 after the F5-F8 integration
static uint8_t s_last_status       = 0;  // latched STATUS (0x93) after both integrations

// Runs one SMUX pass end to end: six ADC channels, plus the STATUS2 byte sampled while
// that integration's flags are still current.
//
// This reproduces Adafruit_AS7341::setSMUXLowChannels() followed by the read half of
// readAllChannels(). It has to, because readAllChannels() runs BOTH integrations before
// it returns and STATUS2 only ever describes the most recent one — so reading status
// after it returns describes the F5-F8 pass and is structurally blind to F1-F4
// saturation. setSMUXCommand() and enableSMUX() are private in the library, but both are
// single register operations, so this reuses the public setup_*() SMUX tables and does
// only those two steps by hand rather than forking the library.
static bool runSmuxPass(Adafruit_AS7341 &dev, bool f1_f4, uint16_t out[6], uint8_t *status2) {
  constexpr int kSmuxTimeoutMs = 1000;

  dev.enableSpectralMeasurement(false);

  if (!updateRegister8(kAs7341Cfg6, kAs7341SmuxCmdMask, kAs7341SmuxCmdWrite)) return false;
  if (f1_f4) {
    dev.setup_F1F4_Clear_NIR();
  } else {
    dev.setup_F5F8_Clear_NIR();
  }

  // Kick the SMUX write, then wait for the chip to clear SMUXEN itself.
  if (!updateRegister8(kAs7341Enable, kAs7341SmuxEnBit, kAs7341SmuxEnBit)) return false;
  bool smux_done = false;
  for (int waited = 0; waited < kSmuxTimeoutMs && !smux_done; waited++) {
    uint8_t enable_val = 0;
    if (!readRegister8(kAs7341Enable, &enable_val)) return false;
    if ((enable_val & kAs7341SmuxEnBit) == 0) {
      smux_done = true;
      break;
    }
    delay(1);
  }
  if (!smux_done) return false;

  if (!dev.enableSpectralMeasurement(true)) return false;
  dev.delayForData(0);  // polls AVALID

  // Sample STATUS2 before anything can start the next integration — these flags describe
  // the integration that just finished and are re-armed by the next one.
  if (!readRegister8(kAs7341Status2, status2)) return false;

  Wire.beginTransmission(kAs7341I2cAddress);
  Wire.write(kAs7341Ch0DataL);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<int>(kAs7341I2cAddress), 12) != 12) return false;
  for (uint8_t i = 0; i < 6; i++) {
    const uint8_t lo = Wire.read();
    const uint8_t hi = Wire.read();
    out[i] = (uint16_t)lo | ((uint16_t)hi << 8);
  }
  return true;
}

bool as7341_readInto(SpectrometerResult *out) {
  if (!out) {
    return false;
  }

  // Clear the latched status so `spec_diag` can also observe whether STATUS (0x93)
  // accumulates ASAT across both passes. Nothing below depends on the answer — the
  // per-pass STATUS2 reads are authoritative — but it is the cheap way to find out
  // whether a future version could drop to a single post-read status check.
  writeRegister8(kAs7341Status, 0xFF);

  uint16_t raw[kAdafruitLen];
  uint8_t status2_low = 0;
  uint8_t status2_high = 0;
  if (!runSmuxPass(as7341, true, &raw[0], &status2_low)) {
    return false;
  }
  if (!runSmuxPass(as7341, false, &raw[6], &status2_high)) {
    return false;
  }

  s_last_status2_low  = status2_low;
  s_last_status2_high = status2_high;
  s_last_status       = 0;
  readRegister8(kAs7341Status, &s_last_status);

  out->model         = SpectrometerModel::AS7341;
  out->channel_count = kResultLen;
  out->sat_mask      = 0;
  out->sat_flags     = 0;
  // First SMUX pass: Adafruit[0..3] -> channels[0..3]
  out->channels[0] = raw[0];
  out->channels[1] = raw[1];
  out->channels[2] = raw[2];
  out->channels[3] = raw[3];
  // raw[4] and raw[5] are SMUX pass-1 clear/NIR intermediates — skipped
  // Second SMUX pass: Adafruit[6..11] -> channels[4..9]
  out->channels[4] = raw[6];
  out->channels[5] = raw[7];
  out->channels[6] = raw[8];
  out->channels[7] = raw[9];
  out->channels[8] = raw[10];
  out->channels[9] = raw[11];

  // Digital: the counter topped out. Full scale follows the exposure, so comparing
  // against a hardcoded 0xFFFF would never fire at short integration times.
  const uint16_t full_scale = as7341FullScale(as7341.getATIME(), as7341.getASTEP());
  for (uint8_t i = 0; i < kResultLen; i++) {
    if (out->channels[i] >= full_scale) {
      out->sat_mask  |= (uint16_t)(1u << i);
      out->sat_flags |= SAT_DIGITAL;
    }
  }

  // Analog: reported device-wide by the hardware, so either pass condemns the reading.
  const uint8_t status2_any = (uint8_t)(status2_low | status2_high);
  if ((status2_any & kAs7341AsatAnalog) != 0)  out->sat_flags |= SAT_ANALOG;
  if ((status2_any & kAs7341AsatDigital) != 0) out->sat_flags |= SAT_DIGITAL;

  return true;
}

uint16_t as7341_getFullScale() {
  return as7341FullScale(as7341.getATIME(), as7341.getASTEP());
}

void as7341_getLastStatusBytes(uint8_t *status, uint8_t *status2_low, uint8_t *status2_high) {
  if (status)       *status       = s_last_status;
  if (status2_low)  *status2_low  = s_last_status2_low;
  if (status2_high) *status2_high = s_last_status2_high;
}

bool as7341_readDiagRegister(uint8_t which, uint8_t *value) {
  // Exposed only for `spec_diag`; `which` is one of the kAs7341* register addresses.
  return readRegister8(which, value);
}

uint8_t as7341_intenabRegister() { return kAs7341Intenab; }

bool as7341_setAtIME(uint8_t atime_value) {
  return as7341.setATIME(atime_value);
}

uint8_t as7341_getAtIME() {
  return as7341.getATIME();
}

bool as7341_setAStep(uint16_t astep_value) {
  return as7341.setASTEP(astep_value);
}

uint16_t as7341_getAStep() {
  return as7341.getASTEP();
}

bool as7341_setGain(as7341_gain_t gain) {
  if (!as7341.setGain(gain)) {
    return false;
  }
  return as7341.getGain() == gain;
}

uint8_t as7341_getGain() {
  return static_cast<uint8_t>(as7341.getGain());
}

// Returns the quantized actual LED current in mA, 0 if disabled, 0xFFFF on error.
uint16_t as7341_setLEDCurrent(uint16_t led_current_ma) {
  if (led_current_ma == 0) {
    as7341.enableLED(false);
    return 0;
  }

  if (!as7341.setLEDCurrent(led_current_ma)) {
    return 0xFFFF;
  }

  // Quantize to device resolution: 4 mA minimum, 2 mA steps.
  uint16_t normalized = led_current_ma < 4 ? 4 : led_current_ma;
  normalized = 4 + (((normalized - 4) / 2) * 2);

  if (as7341.getLEDCurrent() != normalized) {
    return 0xFFFF;
  }

  as7341.enableLED(true);
  return normalized;
}
