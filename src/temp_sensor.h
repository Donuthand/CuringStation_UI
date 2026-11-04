// DS18B20 probe on the 1-Wire bus — implements the sensing half of Section 4
// (Temperature Sensor Decision) and the DS18B20 wiring of Section 10.
//
// Pin: GPIO4 (Section 9 table), with the mandatory 4.7k pull-up to 3.3V. Without
// that resistor the open-drain bus never reads — begin() will report no device.
//
// Reads are non-blocking. A 12-bit DS18B20 conversion takes ~750 ms, which is far
// too long to stall the UI/timer loop, so the driver requests a conversion, returns
// immediately, and picks the result up on a later poll().
#pragma once

#include <Arduino.h>

namespace TempSensor {

// Starts the bus and puts the probe in 12-bit mode (0.0625°C, Section 4).
// Returns false if no DS18B20 answers — check the 4.7k pull-up and the GPIO4 wire.
bool begin();

// Call every loop. Drives the request/read cycle; cheap when nothing is due.
void poll();

// Latest good reading. Returns false if the probe has never answered, returned a
// fault value, or has gone quiet for longer than STALE_MS — treat false as
// "temperature unknown", which Heater::update() turns into heat OFF.
bool read(float& tempC);

// millis() of the last good reading, 0 if there has never been one.
uint32_t lastGoodMillis();

// A reading older than this counts as no reading at all. Three missed ~1 s cycles.
constexpr uint32_t STALE_MS = 5000;

} // namespace TempSensor
