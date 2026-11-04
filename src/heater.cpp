// See heater.h for which spec sections this implements.
#include "heater.h"

static float targetC = Heater::SETPOINT_MIN_C;
static bool heatOn = false;

// One place that touches the pin, so "commanded state" and "pin state" cannot drift.
static void driveSSR(bool on) {
  digitalWrite(Heater::SSR_PIN, on ? HIGH : LOW); // Active HIGH, Section 9
  heatOn = on;
}

namespace Heater {

void begin() {
  pinMode(SSR_PIN, OUTPUT);
  driveSSR(false);
}

float clampSetpoint(float c) {
  if (c < SETPOINT_MIN_C) return SETPOINT_MIN_C;
  if (c > SETPOINT_MAX_C) return SETPOINT_MAX_C;
  return c;
}

float setSetpoint(float c) {
  targetC = clampSetpoint(c);
  return targetC;
}

float setpoint() {
  return targetC;
}

void update(float tempC, bool tempValid) {
  if (!tempValid) {           // unknown temperature is a fault, not a reason to keep heating
    driveSSR(false);
    return;
  }

  // Section 5: on at setpoint - 0.5, off at setpoint + 0.5. Between the two
  // thresholds the state is held, which is what makes it hysteresis.
  if (heatOn) {
    if (tempC >= targetC + HYSTERESIS_C) driveSSR(false);
  } else {
    if (tempC <= targetC - HYSTERESIS_C) driveSSR(true);
  }
}

void forceOff() {
  driveSSR(false);
}

bool isOn() {
  return heatOn;
}

} // namespace Heater
