# Mini PAR meter - minimum tool to get a good numerical sense of light intensity


![vis](./KiCad/miniPAR.png)



# Commands

* "reboot" -> 
    - Response: series of lines with info. 
    - ends with the "Ready" line
* "hello" -> 
    - Response: "MiniPAR,V" + version
* "spec" ->
    - Response: {"spectrometer":{"model":"AS7341","channels":{"f1_415":117,"f2_445":602,"f3_480":732,"f4_515":839,"f5_555":1606,"f6_590":1711,"f7_630":1765,"f8_680":1041,"clear":3704,"nir":359}}}
* "spec,raw" ->
    - Response: {"spectrometer":{"model":"AS7341","channels":{"f1_415":117,"f2_445":602,"f3_480":732,...}}}
    - Returns unscaled channel values (raw counts, not multiplied by PAR coefficients)
* "set_led," + current value in mA ->
    - Response:  {"spectrometer":{"led_current_ma":20}} 
    - Example for 20mA
* "par" -> 
    - Response: 123.45
    - Read back par vale. This is a par_raw scaled by y=ax+b
* "par_raw" ->
    - Response: 1.23 
    - The weighted sum of the channel counts
* "cal_par_slope," + slope -> 
    - Response: {"calibration":{"slope":10.0}}
    - Example for slope = 10.0
    - Set the "a" coefficient for y=ax+b scaling
* "cal_par_intercept," + intercept -> 
    - Response: {"calibration":{"intercept":2.3}}
    - Example for intercept = 2.3
    - Set the "b" coefficient for y=ax+b scaling
* "set_spec_coeff," + channel + "," + value ->
    - Response: {"spectrometer_coeff":{"channel":0,"value":0.018182}}
    - Example: "set_spec_coeff,0,0.018182"
    - Set per-channel PAR coefficient (channel 0-17, up to 6 decimal places)
    - Values are persisted to EEPROM
* "get_spec_coeff" ->
    - Response: {"spectrometer_coeffs":{"channels":{"0":0.018182,"1":0.009091,"2":0.004673,...}}}
    - Get all 18 per-channel PAR coefficients
* "get_spec_coeff," + channel ->
    - Response: {"spectrometer_coeff":{"channel":0,"value":0.018182}}
    - Example: "get_spec_coeff,0"
    - Get specific per-channel PAR coefficient
* "set_name," + string
    - response: {"device_name":"Lukasz"}
    - Set custom device name (persisted to EEPROM)
* "get_name" 
    - response: {"device_name":"Lukasz"}
    - Get current device name
    

# Protocol 

* terminator: "\r"
* PAR units - µmol/m²/s"


FW Design (L. Grabowski, L. Caracciolo),

HW Design (L. Grabowski)
