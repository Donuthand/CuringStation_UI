/*  Bring-up sketch for the DS18B20 + SSR loop.
    Build Order step 2 (DS18B20) and step 5 (transistor + SSR-25DA).
    Serial Monitor at 115200.

    Build:
      arduino-cli compile --fqbn "esp32:esp32:esp32:UploadSpeed=115200" \
        --library "D:/COOPProject/Coding/Firmware" --upload -p COM3 .
      (hold the BOOT button while esptool prints its "Connecting..." dots)

    The build order does the sensor (step 2) before the switching (step 5), so run
    this first with NOTHING wired to GPIO16 — the pin toggles, but it drives nothing,
    and the printed ON/off lets the hysteresis be watched by warming the probe with
    a hand. Only wire the transistor + SSR once that looks right, and per Section 17
    switch a LAMP first. No mains heating mat until every subsystem is signed off.
*/

#include <temp_sensor.h>
#include <heater.h>

static constexpr float TEST_SETPOINT_C = 40.0f; // inside the 40-50 lock
static constexpr uint32_t PRINT_INTERVAL_MS = 1000;

static uint32_t lastPrintMs = 0;

void setup() {
  // First, before anything slow: the mat must never be live during boot.
  Heater::begin();

  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("########  DS18B20 + SSR CHECK  ########");
  Serial.println("GPIO16 switches for real. Nothing on it yet = harmless; SSR on it = lamp only.");

  float sp = Heater::setSetpoint(TEST_SETPOINT_C);
  Serial.print("Setpoint: "); Serial.print(sp);
  Serial.print(" C   (on below "); Serial.print(sp - Heater::HYSTERESIS_C);
  Serial.print(", off above "); Serial.print(sp + Heater::HYSTERESIS_C);
  Serial.println(")");

  if (!TempSensor::begin()) {
    Serial.println(">> No DS18B20 found on GPIO4. Check, in this order:");
    Serial.println("   1. the 4.7k pull-up from the data line to 3.3V (mandatory)");
    Serial.println("   2. data wire on GPIO4");
    Serial.println("   3. red to 3.3V, black to GND -- NOT 5V");
    Serial.println("   The sketch keeps running; heat stays off while temp is unknown.");
  } else {
    Serial.println("DS18B20 found, 12-bit mode. First reading in ~1s.");
  }
}

void loop() {
  TempSensor::poll();

  float tempC = 0.0f;
  bool valid = TempSensor::read(tempC);

  Heater::update(tempC, valid);

  if (millis() - lastPrintMs >= PRINT_INTERVAL_MS) {
    lastPrintMs = millis();
    if (valid) {
      Serial.print("temp "); Serial.print(tempC, 2); Serial.print(" C   heat ");
      Serial.println(Heater::isOn() ? "ON" : "off");
    } else {
      Serial.println("temp UNKNOWN (no/stale reading)   heat off -- failsafe");
    }
  }
}
