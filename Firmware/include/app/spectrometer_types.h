#pragma once
#include <stdint.h>

enum class SpectrometerModel : uint8_t {
  None,
  AS7341,
  AS7343,
  ProbePendingAt0x39,
  UnknownAt0x39,
};

// The two saturation mechanisms are physically distinct and must not be conflated:
// the ADC counter topping out is per-channel and derivable from the counts, while the
// analog front end clipping is reported device-wide by the hardware ASAT bit and can
// happen at counts well below full scale.
enum SatFlag : uint8_t {
  SAT_ANALOG  = 1 << 0,  // hardware ASAT: photodiode/integrator clipped. Device-wide.
  SAT_DIGITAL = 1 << 1,  // at least one channel reached ADC full scale. See sat_mask.
};

struct SpectrometerResult {
  SpectrometerModel model;
  uint8_t  channel_count;  // number of valid entries in channels[]
  uint16_t channels[18];   // indexed 0..channel_count-1; 18 = AS7343 bringup max
  uint16_t sat_mask;       // bit N set => channels[N] reached ADC full scale (digital only)
  uint8_t  sat_flags;      // OR of SatFlag; the only place analog saturation is reported
};

// True if the reading is compromised by either mechanism.
inline bool spectrometerResultSaturated(const SpectrometerResult &r) {
  return r.sat_flags != 0 || r.sat_mask != 0;
}

