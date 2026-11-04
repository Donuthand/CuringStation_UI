// SSR-25DA heat control — implements Section 5 (Hysteresis Control), the control
// side of Section 6 (transistor driver), and the 40-50°C lock of Section 19.
//
// Output is GPIO16, **Active HIGH** (Section 9): HIGH turns the NPN on, which pulls
// SSR terminal 4(-) to GND and switches the mat on. begin() drives it LOW before
// anything else runs, and must be the first call in setup().
//
// The mat is only ever commanded from update(), and update() turns it off whenever
// the temperature is unknown. The 3A fuse and the mat's own built-in thermostat
// (Section 7) are the hardware layers underneath this and do not depend on any of
// it. Rev 8 removed the separate 72°C TCO that used to sit in that chain, so the
// mat's thermostat is now the ONLY over-temperature protection — its trip point is
// unmeasured (open action item 3) and the safety case rests on it.
#pragma once

#include <Arduino.h>

namespace Heater {

constexpr uint8_t SSR_PIN = 16;             // Section 9 GPIO table
constexpr float SETPOINT_MIN_C = 40.0f;     // Section 19: range locked in code (40 since 2026-10-12: service manual says 40-50)
constexpr float SETPOINT_MAX_C = 50.0f;
constexpr float HYSTERESIS_C = 0.5f;        // Section 5: on at SP-0.5, off at SP+0.5

// Forces GPIO16 LOW. Call first in setup(), before anything slow (display init,
// sensor init) can leave the pin floating.
void begin();

// Clamps to SETPOINT_MIN_C..SETPOINT_MAX_C. Section 19 says the range is locked in
// code, so out-of-range input is clamped rather than trusted.
float clampSetpoint(float c);

// Stores the (clamped) target. Returns the value actually stored, so a caller can
// tell the user their 60 became 50.
float setSetpoint(float c);
float setpoint();

// Call every loop with the latest reading. tempValid comes straight from
// TempSensor::read() — false means the probe is missing, faulted or stale, and the
// mat is switched off unconditionally.
void update(float tempC, bool tempValid);

// Immediate off, independent of temperature. For alarm-stop / fault paths.
void forceOff();

bool isOn();

} // namespace Heater
