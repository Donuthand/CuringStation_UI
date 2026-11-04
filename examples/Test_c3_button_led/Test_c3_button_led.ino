/*  Bring-up test for the remote button's own wiring on the ESP32-C3 Super Mini: hold the
    button, the LED is on; let go, it is off. No radio, no sleep — checks the button and
    LED wiring on their own, before blaming Test_espnow_button (2026-10-16, Dough: "test
    button code when press the led on").

    Same pins and wiring as Test_espnow_button (Paper/Drawings/ECS-BTN-01_remote_wiring.svg):
      - Button between GPIO3 and GND (+ the 10 kOhm pull-up GPIO3 -> 3V3; the internal
        pull-up is on too, so the test also works on a breadboard without it).
      - GPIO4 -> 330 Ohm -> LED anode (long leg); cathode -> GND.

    Standalone sketch, no library. MUST include :CDCOnBoot=cdc (see Test_c3_led's header):

      arduino-cli compile --fqbn "esp32:esp32:nologo_esp32c3_super_mini:CDCOnBoot=cdc" --upload -p COMx .

    This REPLACES the remote code on that C3: flash Test_espnow_button back afterwards.
    A C3 already running Test_espnow_button is asleep with no USB port: plug USB in (a cold
    power-up stays awake a moment) or hold BOOT, tap RESET, release BOOT, then upload.

    Serial at 115200 prints each press and release, in case the LED itself is the fault.
*/

constexpr uint8_t BUTTON_PIN = 3;
constexpr uint8_t LED_PIN = 4;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("=== C3 button + LED test: hold the button, LED on ===");
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
}

void loop() {
  static bool wasPressed = false;
  static uint32_t presses = 0;

  bool pressed = (digitalRead(BUTTON_PIN) == LOW);   // pull-up: pressed = LOW
  digitalWrite(LED_PIN, pressed ? HIGH : LOW);
  if (pressed != wasPressed) {
    wasPressed = pressed;
    if (pressed) presses++;
    Serial.printf("[button] %s (presses %lu)\n", pressed ? "PRESSED" : "released", (unsigned long)presses);
  }
  delay(10);   // also a crude debounce for the printout
}
