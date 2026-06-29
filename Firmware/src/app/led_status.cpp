#include <Arduino.h>
#include "app/led_status.h"
#include "app/device_config.h"

static bool     s_detected       = false;
static bool     s_saturated      = false;
static bool     s_ledState       = false;
static uint32_t s_lastToggleMs   = 0;

// 20 Hz blink → 50 ms period → toggle every 25 ms
static constexpr uint32_t kBlinkHalfPeriodMs = 50;

static void applyLed(bool on) {
  s_ledState = on;
  digitalWrite(DEVICE_STATUS_LED_PIN, on ? HIGH : LOW);
}

void ledStatusInit() {
  pinMode(DEVICE_STATUS_LED_PIN, OUTPUT);
  applyLed(false);
}

void ledStatusSetDetected(bool detected) {
  s_detected = detected;
  if (!detected) {
    applyLed(false);
  } else if (!s_saturated) {
    applyLed(true);
  }
  // If saturated, ledStatusUpdate() drives the blink
}

void ledStatusSetSaturated(bool saturated) {
  if (s_saturated == saturated) return;
  s_saturated = saturated;
  if (!s_detected) return;
  if (!saturated) {
    applyLed(true);  // back to solid on
  }
  // If now saturating, ledStatusUpdate() starts blinking from next loop tick
}

void ledStatusUpdate() {
  if (!s_detected || !s_saturated) return;
  const uint32_t now = millis();
  if ((now - s_lastToggleMs) >= kBlinkHalfPeriodMs) {
    s_lastToggleMs = now;
    applyLed(!s_ledState);
  }
}
