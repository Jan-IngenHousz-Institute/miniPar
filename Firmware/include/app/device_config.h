#pragma once

// ---------------------------------------------------------------------------
// Device identity — fixed at build time
// ---------------------------------------------------------------------------
#define DEVICE_PRODUCT_NAME     "MiniPAR"
#define DEVICE_VERSION          "1.1"
#define DEVICE_FIRMWARE_VERSION      1.04f
#define DEVICE_FIRMWARE_VERSION_STR  "1.04"

// device ID is a unique identifier for the device, derived from the the MAC address

// Sentinel appended to every JSON output frame (CRC-like footer)
#define DEVICE_FRAME_FOOTER     "7A1E3AA1"

// ---------------------------------------------------------------------------
// Hardware / communication
// ---------------------------------------------------------------------------
#define DEVICE_SERIAL_BAUD      115200
#define DEVICE_I2C_SDA_PIN      3
#define DEVICE_I2C_SCL_PIN      4
#define DEVICE_STATUS_LED_PIN   10

// ---------------------------------------------------------------------------
// Configurable parameters — stored in NVS; these are the compile-time defaults
// ---------------------------------------------------------------------------
#define DEVICE_DEFAULT_NAME     "NoName"
#define DEVICE_NAME_MAX_LEN     20
