/*  Bring-up test for an EXTERNAL LED on the ESP32-C3 Super Mini — run this BEFORE
    Test_espnow_button, since that sketch's LED_PIN/LED_ACTIVE_LOW need to match
    whatever this test confirms.

    Switched 2026-09-14 from the onboard LED (GPIO8 guess, unconfirmed one way or
    the other because Serial never came up reliably enough to be sure) to a plain
    external LED on GPIO4, wired active-HIGH. See the wiring in the LED_PIN comment
    below. This sidesteps the onboard-LED pin/polarity guess entirely.

    Build target: "Nologo ESP32C3 Super Mini" (fqbn esp32:esp32:nologo_esp32c3_super_mini).
    Standalone sketch, no library needed. MUST include :CDCOnBoot=cdc — this board's
    boards.txt entry mislabels its default USB-CDC-on-boot option (both menu choices
    show as "Enabled" in `arduino-cli board details`), and the actual default leaves
    Serial silently going nowhere with no error, even though the sketch itself runs
    fine (confirmed 2026-09-14: LED worked, Serial showed nothing, on both this
    sketch and Test_espnow_button, in both Arduino IDE and `arduino-cli monitor`):

      arduino-cli compile --fqbn "esp32:esp32:nologo_esp32c3_super_mini:CDCOnBoot=cdc" --upload -p COMx .

    What to watch: the external LED should blink on/off once a second. If nothing
    blinks, double check the resistor/LED orientation before suspecting the pin —
    GPIO4 is a plain general-purpose pin with nothing special about it.

    Serial also prints each transition at 115200, in case the LED itself is missing
    or hard to see.
*/

// External LED on GPIO4: GPIO4 -> 220-330 ohm resistor -> LED anode (long leg);
// LED cathode (short leg) -> GND. Wired this way it's active-HIGH, unlike the
// onboard LED guess this sketch used before. GPIO4 chosen because it's free, not a
// strapping pin (2/8/9) and not USB D+/D- (18/19).
constexpr uint8_t LED_PIN = 4;
constexpr bool LED_ACTIVE_LOW = false;
constexpr uint32_t BLINK_MS = 1000;

static void ledSet(bool on) {
  digitalWrite(LED_PIN, (on != LED_ACTIVE_LOW) ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("=== C3 Super Mini onboard LED test ===");
  Serial.print("LED_PIN=");
  Serial.print(LED_PIN);
  Serial.print(" LED_ACTIVE_LOW=");
  Serial.println(LED_ACTIVE_LOW ? "true" : "false");

  pinMode(LED_PIN, OUTPUT);
  ledSet(false);
}

void loop() {
  static bool on = false;
  static uint32_t lastMs = 0;

  uint32_t now = millis();
  if (now - lastMs >= BLINK_MS) {
    lastMs = now;
    on = !on;
    ledSet(on);
    Serial.println(on ? "[led] ON" : "[led] off");
  }
}
