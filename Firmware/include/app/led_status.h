#pragma once

// Status LED driver (GPIO DEVICE_STATUS_LED_PIN).
//
// States:
//   Off            — no spectrometer detected at boot
//   Solid on       — spectrometer detected, all channels in range
//   20 Hz blink    — spectrometer detected, at least one channel saturated
//
// Call ledStatusUpdate() on every loop() iteration for the blink to work.

void ledStatusInit();
void ledStatusSetDetected(bool detected);
void ledStatusSetSaturated(bool saturated);
void ledStatusUpdate();
