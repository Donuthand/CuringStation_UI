/*  Driver-stage bring-up sketch — Build Order step 5, part 10a.
    Serial Monitor at 115200.

    Build:
      arduino-cli compile --fqbn "esp32:esp32:esp32:UploadSpeed=115200" \
        --library "D:/COOPProject/Coding/Firmware" --upload -p COM3 .
      (hold the BOOT button while esptool prints its "Connecting..." dots)

    This exists so the transistor + SSR control stage can be watched on a multimeter
    without warming the DS18B20 by hand for every transition. It ignores temperature
    entirely and just walks GPIO16 on a slow fixed cycle.

    RUN THIS WITH SSR TERMINALS 1 AND 2 BARE. No load, no mains. Meter on DC volts
    across SSR 3(+) and 4(-):
        expect ~0V while this prints "off", ~5V while it prints "ON".

    Then, with the meter still attached, press EN. Terminals 3-4 must never blip to
    5V during boot -- that is the property Heater::begin()-first-in-setup() exists to
    guarantee. Note there is no base pulldown fitted, so that guarantee currently
    depends on the ESP32 being alive; this test is what says whether it holds.
*/

#include <heater.h>

static constexpr uint32_t HALF_PERIOD_MS = 3000; // slow enough to read a moving meter

static uint32_t lastToggleMs = 0;
static bool on = false;

void setup() {
  // First, before anything slow: the SSR must never be live during boot.
  Heater::begin();

  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("########  SSR DRIVER TOGGLE  ########");
  Serial.println("GPIO16 switches for real. Terminals 1/2 must be BARE -- no load, no mains.");
  Serial.print("Cycling every "); Serial.print(HALF_PERIOD_MS); Serial.println(" ms.");

  pinMode(Heater::SSR_PIN, OUTPUT);
  digitalWrite(Heater::SSR_PIN, LOW);
  lastToggleMs = millis();
}

void loop() {
  if (millis() - lastToggleMs < HALF_PERIOD_MS) return;

  lastToggleMs = millis();
  on = !on;
  digitalWrite(Heater::SSR_PIN, on ? HIGH : LOW);
  Serial.println(on ? "GPIO16 HIGH  -> expect ~5V across SSR 3-4   [ON]"
                    : "GPIO16 LOW   -> expect ~0V across SSR 3-4   [off]");
}
