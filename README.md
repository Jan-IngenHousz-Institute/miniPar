# Mini PAR meter - minimum tool to get a good numerical sense of light intensity


![vis](./KiCad/miniPAR.png)



# Commands

Commands are sent as plain text terminated with `\n`. JSON mode is also supported — send a valid JSON object and the device will respond in JSON.

* `hello`
    - Response: `MiniPAR,1.1` (plain) / `{"device":"MiniPAR","version":"1.1"}` (JSON)
* `reboot`
    - Restarts the device. Prints boot info ending with `Ready`.
* `delay_ms,<value>`
    - Example: `delay_ms,500`
    - Blocks for `<value>` milliseconds, then responds with the delay value: `500` (plain) / `{"delay_ms":500}` (JSON). Missing/negative value is treated as `0`.
* `battery`
    - Response: `NaN` (plain) / `{"battery":"NaN"}` (JSON)
    - Battery measurement not yet implemented.
* `i2c_scan`
    - Response: comma-separated list of I2C addresses that ACK (e.g. `0x39`)
* `spec`
    - Response: `{"spectrometer":{"model":"AS7341","channels":{"f1_415":1.2345,"f2_445":6.7890,...}}}`
    - Returns **basic counts** per channel: `raw_reading / gain / integration_time` (counts per second per gain unit). Values are comparable across different gain and integration-time settings.
* `spec_raw`
    - Response: same format as `spec` but raw unscaled integer counts (no normalization applied).
* `spec_flash,<mA>`
    - Example: `spec_flash,10`
    - Dark read → LED on → lit read. Response: `dark:<values>;lit:<values>;diff:<values>` (plain).
* `spec_set_atime,<0-255>`
    - Set spectrometer ATIME integration register. Response: the value set.
* `spec_set_astep,<0-65534>`
    - Set spectrometer ASTEP register. Response: the value set.
* `spec_set_gain,<0-10 (AS7341) | 0-12 (AS7343)>`
    - Set spectrometer gain index. Response: the value set.
* `spec_status` or `status`
    - Response: `model=<name>,available=<0|1>,atime=<val>,astep=<val>,gain=<val>` (plain)
* `set_led,<mA>`
    - Example: `set_led,20`
    - Response: `{"spectrometer":{"led_current_ma":20}}` (JSON) / actual mA set (plain)
    - LED current capped at 20 mA.
* `par`
    - Response: `123.45`
    - PAR value in µmol/m²/s, scaled by linear calibration: `y = a·par_raw + b`
* `par_raw`
    - Response: `1.23`
    - Weighted dot product of **basic counts** (raw / gain / integration_time) and per-channel PAR coefficients. Independent of gain and integration-time settings.
* `cal_par_slope,<value>`
    - Example: `cal_par_slope,10.0`
    - Response: `{"calibration":{"slope":10.0}}`
    - Set the `a` coefficient for `y = ax + b` scaling. Persisted to NVS.
* `cal_par_intercept,<value>`
    - Example: `cal_par_intercept,2.3`
    - Response: `{"calibration":{"intercept":2.3}}`
    - Set the `b` coefficient for `y = ax + b` scaling. Persisted to NVS.
* `set_spec_coeff,<channel>,<value>`
    - Example: `set_spec_coeff,0,0.614975`
    - Response: `{"spectrometer_coeff":{"channel":0,"value":0.614975}}`
    - Set per-channel PAR coefficient (channel 0–17). Coefficients are applied to **basic counts** (raw / gain / integration_time), so they must account for the desired normalization. Persisted to NVS.
    - Default coefficients are pre-scaled for the default AS7341 settings (ATIME=100, ASTEP=999, GAIN=2×); re-derive from calibration if defaults are changed.
* `get_spec_coeff`
    - Response: `{"spectrometer_coeffs":{"channels":{"0":0.614975,"1":0.053037,...}}}`
    - Get all 18 per-channel PAR coefficients.
* `get_spec_coeff,<channel>`
    - Example: `get_spec_coeff,0`
    - Response: `{"spectrometer_coeff":{"channel":0,"value":0.614975}}`
    - Get a specific per-channel PAR coefficient.
* `set_name,<string>`
    - Response: `{"device_name":"MyDevice"}`
    - Set custom device name. Persisted to NVS.
* `get_name`
    - Response: `{"device_name":"MyDevice"}`
    - Get current device name.

## BME280 (optional temperature/pressure/humidity sensor)

Detected automatically at boot (I2C address `0x76` or `0x77`); all commands below respond with `error:not_available` (plain) / `{"bme280":{"error":"not_available"}}` (JSON) if no BME280 is fitted. Temperature, pressure, and humidity are all enabled by default, at the highest oversampling (x16), IIR filter off, one-shot read mode.

* `bme_status`
    - Response (plain): `continuous=<0|1>,continuous_active=<0|1>,temp_comp=<value>,temp_en=<0|1>,temp_osrs=<0-5>,press_en=<0|1>,press_osrs=<0-5>,hum_en=<0|1>,hum_osrs=<0-5>,iir=<0-4>`
    - Response (JSON): `{"bme280_status":{"available":true,"continuous_mode":false,"continuous_active":false,"temperature_compensation_c":0.00,"temperature":{"enabled":true,"oversampling":5},"pressure":{"enabled":true,"oversampling":5},"humidity":{"enabled":true,"oversampling":5},"iir_filter":0}}`
* `bme_set_temp_enable,<0|1>`
    - Enable/disable temperature in `bme_get_tph` output. Temperature is still sampled internally regardless (pressure/humidity compensation depend on it) — this only affects reporting.
* `bme_set_press_enable,<0|1>`
    - Enable/disable pressure in `bme_get_tph` output.
* `bme_set_hum_enable,<0|1>`
    - Enable/disable humidity in `bme_get_tph` output.
* `bme_set_temp_oversample,<0-5>`
    - Oversampling: `0`=off (clamped to `1`, since temperature must always be sampled), `1`=x1, `2`=x2, `3`=x4, `4`=x8, `5`=x16 (default).
* `bme_set_press_oversample,<0-5>`
    - Same scale as above; `0`=channel skipped by the sensor.
* `bme_set_hum_oversample,<0-5>`
    - Same scale as above; `0`=channel skipped by the sensor.
* `bme_set_iir,<0-4>`
    - IIR filter coefficient: `0`=off (default), `1`=x2, `2`=x4, `3`=x8, `4`=x16.
* `bme_set_continuous,<0|1>`
    - `0` (default) = one-shot: every `bme_get_tph` call takes its own forced-mode measurement.
    - `1` = continuous: the chip free-runs in normal mode; `bme_get_tph` blocks only on the first call after enabling (waiting for that first conversion), then returns whatever the chip's background cycle last produced.
* `bme_set_temp_comp,<value>`
    - Set the temperature compensation offset in °C, added to every raw temperature reading (and folded into the pressure/humidity compensation math) to null out self-heating bias. Persisted to NVS.
* `bme_get_temp_comp`
    - Response: current temperature compensation offset in °C.
* `bme_get_tph` (alias: `tph`)
    - Response (plain): `<temperature_c>,<pressure_hpa>,<humidity_pct>` — positional CSV, blank field for a disabled channel.
    - Response (JSON): `{"bme280":{"temperature_c":23.45,"pressure_hpa":1013.25,"humidity_pct":45.20}}` — `null` for a disabled channel.
    - Behavior depends on `bme_set_continuous`: one-shot mode (default) takes a fresh forced-mode reading every call; continuous mode waits for the first conversion only on the first call after being enabled, then returns the most recent free-running reading on every call after that.


# Status LED (GPIO 10)

The LED on GPIO 10 provides a visual indication of spectrometer state:

| LED state | Meaning |
|-----------|---------|
| Off | No spectrometer detected at boot |
| Solid on | Spectrometer detected, all channels within range |
| Blinking 20 Hz | At least one spectrometer channel is saturated |

Saturation is checked automatically on every spectrometer read (`spec`, `spec_raw`, `par`, `par_raw`, etc.). The LED returns to solid-on as soon as the next read finds all channels within range. Reduce gain or integration time (`spec_set_gain`, `spec_set_atime`, `spec_set_astep`) to clear saturation.


# Protocol

* Command terminator: `\n` (`\r` is ignored)
* PAR units: µmol/m²/s
* **Basic counts**: `raw_reading / gain / integration_time`, where `integration_time = (ATIME+1) × (ASTEP+1) × 2.78 µs`. Used by `spec`, `par`, and `par_raw` — values are gain/integration-time independent.
* **Default AS7341 settings**: ATIME=100, ASTEP=999, GAIN=2× (integration time ≈ 280.8 ms). Per-channel PAR coefficients stored in NVS are calibrated for basic counts at these defaults.


FW Design (L. Grabowski, L. Caracciolo),

HW Design (L. Grabowski)
