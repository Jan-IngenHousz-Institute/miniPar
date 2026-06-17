# Mini PAR meter - minimum tool to get a good numerical sense of light intensity


![vis](./KiCad/miniPAR.png)



# Commands

Commands are sent as plain text terminated with `\n`. JSON mode is also supported — send a valid JSON object and the device will respond in JSON.

* `hello`
    - Response: `MiniPAR,1.1` (plain) / `{"device":"MiniPAR","version":"1.1"}` (JSON)
* `reboot`
    - Restarts the device. Prints boot info ending with `Ready`.
* `battery`
    - Response: `NaN` (plain) / `{"battery":"NaN"}` (JSON)
    - Battery measurement not yet implemented.
* `i2c_scan`
    - Response: comma-separated list of I2C addresses that ACK (e.g. `0x39`)
* `spec`
    - Response: `{"spectrometer":{"model":"AS7341","channels":{"f1_415":117,"f2_445":602,...}}}`
    - Channel counts multiplied by per-channel PAR coefficients.
* `spec_raw`
    - Response: same format as `spec` but raw unscaled counts (no PAR coefficients applied).
* `spec_flash,<mA>`
    - Example: `spec_flash,10`
    - Dark read → LED on → lit read. Response: `dark:<values>;lit:<values>;diff:<values>` (plain).
    - LED current capped at 20 mA.
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
    - PAR value scaled by linear calibration: `y = a·par_raw + b`
* `par_raw`
    - Response: `1.23`
    - Weighted sum of channel counts using per-channel PAR coefficients.
* `cal_par_slope,<value>`
    - Example: `cal_par_slope,10.0`
    - Response: `{"calibration":{"slope":10.0}}`
    - Set the `a` coefficient for `y = ax + b` scaling. Persisted to NVS.
* `cal_par_intercept,<value>`
    - Example: `cal_par_intercept,2.3`
    - Response: `{"calibration":{"intercept":2.3}}`
    - Set the `b` coefficient for `y = ax + b` scaling. Persisted to NVS.
* `set_spec_coeff,<channel>,<value>`
    - Example: `set_spec_coeff,0,0.018182`
    - Response: `{"spectrometer_coeff":{"channel":0,"value":0.018182}}`
    - Set per-channel PAR coefficient (channel 0–17). Persisted to NVS.
* `get_spec_coeff`
    - Response: `{"spectrometer_coeffs":{"channels":{"0":0.018182,"1":0.009091,...}}}`
    - Get all 18 per-channel PAR coefficients.
* `get_spec_coeff,<channel>`
    - Example: `get_spec_coeff,0`
    - Response: `{"spectrometer_coeff":{"channel":0,"value":0.018182}}`
    - Get a specific per-channel PAR coefficient.
* `set_name,<string>`
    - Response: `{"device_name":"MyDevice"}`
    - Set custom device name. Persisted to NVS.
* `get_name`
    - Response: `{"device_name":"MyDevice"}`
    - Get current device name.


# Protocol

* Command terminator: `\n` (`\r` is ignored)
* PAR units: µmol/m²/s


FW Design (L. Grabowski, L. Caracciolo),

HW Design (L. Grabowski)
