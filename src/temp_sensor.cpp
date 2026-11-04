// See temp_sensor.h for which spec sections this implements.
#include "temp_sensor.h"
#include <OneWire.h>
#include <DallasTemperature.h>

static constexpr uint8_t ONE_WIRE_PIN = 4;      // Section 9 GPIO table
static constexpr uint32_t CONVERSION_MS = 800;  // 12-bit needs ~750 ms; 800 for margin

static OneWire oneWire(ONE_WIRE_PIN);
static DallasTemperature dallas(&oneWire);
static DeviceAddress probeAddress;

static bool haveProbe = false;
static bool conversionPending = false;
static uint32_t requestedAtMs = 0;

static float lastTempC = 0.0f;
static uint32_t lastGoodMs = 0;

namespace TempSensor {

bool begin() {
  dallas.begin();
  haveProbe = dallas.getAddress(probeAddress, 0);
  if (!haveProbe) return false;

  dallas.setResolution(probeAddress, 12);
  dallas.setWaitForConversion(false); // non-blocking, see header
  dallas.requestTemperatures();
  requestedAtMs = millis();
  conversionPending = true;
  return true;
}

void poll() {
  if (!haveProbe || !conversionPending) return;
  if (millis() - requestedAtMs < CONVERSION_MS) return;

  float c = dallas.getTempC(probeAddress);
  conversionPending = false;

  // DEVICE_DISCONNECTED_C is -127. 85.00 is the DS18B20's power-on scratchpad value:
  // it means the probe reset mid-conversion (usually a loose data line or a brown-out),
  // not a real 85°C. Either way it is outside the 40-50°C working range, so rejecting
  // it fails in the safe direction — unknown temperature means heat off.
  bool ok = (c != DEVICE_DISCONNECTED_C) && (c != 85.0f);
  if (ok) {
    lastTempC = c;
    lastGoodMs = millis();
    if (lastGoodMs == 0) lastGoodMs = 1; // 0 is the "never read" sentinel
  }

  dallas.requestTemperatures();
  requestedAtMs = millis();
  conversionPending = true;
}

bool read(float& tempC) {
  if (lastGoodMs == 0) return false;
  if (millis() - lastGoodMs > STALE_MS) return false;
  tempC = lastTempC;
  return true;
}

uint32_t lastGoodMillis() {
  return lastGoodMs;
}

} // namespace TempSensor
