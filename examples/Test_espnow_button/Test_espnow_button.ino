/*  ESP-NOW alarm-stop button — for an ESP32-C3 Super Mini, separate from the main
    station board. Press the button, the station's oldest sounding alarm goes quiet.
    Supersedes the BLE button design in epoxy_curing_station_full.md Section 11.

    Build target: "Nologo ESP32C3 Super Mini" (fqbn esp32:esp32:nologo_esp32c3_super_mini).
    This is a DIFFERENT board than the main station, so it is its OWN sketch/upload,
    not part of Test_ui and NOT using the EpoxyCuringStation library — no --library
    flag, and no TFT_eSPI/keypad/sound baggage this tiny board doesn't need.

    MUST include :CDCOnBoot=cdc — see Test_c3_led's header for why (this board's
    default leaves Serial silently dead with no error, confirmed 2026-09-14):

      arduino-cli compile --fqbn "esp32:esp32:nologo_esp32c3_super_mini:CDCOnBoot=cdc" --upload -p COMx .

    This folder carries its OWN copy of remote_stop_protocol.h (same file as the
    library's, see that header's comment) rather than reaching into the library for
    it, precisely to avoid pulling the rest of the library in.

    (COM port, not 115200/BOOT-hold — those quirks are specific to the main board's
    ESP32-WROOM and its strapping-pin TFT. Dough builds and flashes this too, per
    CLAUDE.md.)

    BATTERY + DEEP SLEEP (2026-10-06, Dough: "yes write deep sleep, battery 902025 400mAH").
    CODE ONLY until flashed. The button sleeps; a press wakes it, it sends one STOP_ALARM,
    stays awake AWAKE_AFTER_PRESS_MS for a quick second press, then sleeps again. Always-on
    (the 2026-09-14 prototype) kept the radio up and would empty a 400 mAh cell in hours.

    Wiring (per unit):
      - Button between GPIO3 and GND, PLUS a 10 kOhm pull-up GPIO3 -> 3V3. Moved off GPIO9:
        on the C3 only GPIO0-5 can wake it from deep sleep, and GPIO9 is a strapping pin
        (held low at power-up = download mode). esp_deep_sleep_start() sets the internal
        pull-up by itself, but Espressif's esp_sleep.h "strongly recommend[s]" an external
        pull-up for wake-on-LOW — a floating pin would wake the board at random. The onboard
        BOOT button (GPIO9) no longer stops alarms.
      - LiPo 902025 (400 mAh) -> TP4056 module WITH protection (B+/B-, OUT+/OUT-) ->
        slide switch on OUT+ -> the C3's 5V pin; OUT- -> GND. Never the 3V3 pin (a full
        LiPo is 4.2 V). Charge through the TP4056's own USB (USB-A to C cable only). Its
        default is 1 A; kept as shipped after a closed-box charge test (2026-10-15, cell peak
        39.2 C) — charge in an air-conditioned room.
      - Switch the battery OFF before plugging USB into the C3 (no back-feeding).

    Flashing a sleeping button: the USB port vanishes while it sleeps. Switch the battery
    off, plug USB in — a cold power-up stays awake AWAKE_AFTER_POWER_ON_MS, long enough to
    start the upload. If that window is missed: hold BOOT, tap RESET, release BOOT.

    LED feedback on an EXTERNAL LED (2026-10-06, Dough: the onboard one is hidden inside a
    case): GPIO4 -> 330 Ohm -> LED anode (long leg), cathode -> GND. Two blinks = powered up;
    one blink = STOP_ALARM sent. Wiring figure: Paper/Drawings/ECS-BTN-01_remote_wiring.svg.

    Sends a broadcast ESP-NOW packet (no pairing, no station MAC needed) — wire format
    in this folder's own remote_stop_protocol.h, kept in sync by hand with the
    library's copy the station uses (remote_stop.cpp). ESP-NOW is peer-to-peer over
    the WiFi radio; it needs no router/AP, so the department having no WiFi network
    does not matter. Sent ONCE per press: the station stops one alarm per packet it
    hears, so repeating the packet for safety would stop several queued alarms.
*/

#include <string.h>

#include <WiFi.h>
#include <esp_now.h>
#include <esp_sleep.h>
#include <esp_wifi.h>

#include "remote_stop_protocol.h"

using namespace RemoteStopProtocol;

constexpr uint8_t BUTTON_PIN = 3;      // GPIO0-5 only can wake a C3 from deep sleep
constexpr uint32_t DEBOUNCE_MS = 200;  // clear of a fast deliberate double-press

// How long to stay awake before sleeping again. After a press: room for a second press
// (two alarms queued) without paying the wake-up delay again. After power-up: long enough
// to open the Serial Monitor or start an upload.
constexpr uint32_t AWAKE_AFTER_PRESS_MS = 3000;
constexpr uint32_t AWAKE_AFTER_POWER_ON_MS = 30000;

// External LED on GPIO4, active-HIGH — confirmed working 2026-09-14 as the fallback for
// the onboard LED (GPIO8, active-LOW), which a case hides. Through 330 Ohm: ~4 mA, and
// only for the 80 ms blinks, so it costs the battery nothing worth counting.
constexpr uint8_t LED_PIN = 4;
constexpr bool LED_ACTIVE_LOW = false;

const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint32_t lastPressMs = 0;
static uint32_t awakeUntilMs = 0;
static bool lastButtonState = HIGH;   // pull-up: HIGH = not pressed

static void ledSet(bool on) {
  digitalWrite(LED_PIN, (on != LED_ACTIVE_LOW) ? HIGH : LOW);
}

static void blink(uint8_t times) {
  for (uint8_t i = 0; i < times; i++) {
    ledSet(true);
    delay(80);
    ledSet(false);
    if (i + 1 < times) delay(120);
  }
}

static void sendStopAlarm() {
  Packet pkt;
  memcpy(pkt.magic, MAGIC, sizeof(MAGIC));
  pkt.version = VERSION;
  pkt.cmd = CMD_STOP_ALARM;

  esp_err_t result = esp_now_send(BROADCAST_ADDR, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
  if (result == ESP_OK) {
    Serial.println("[button] STOP_ALARM sent");
  } else {
    Serial.print("[button] esp_now_send failed: ");
    Serial.println((int)result);
  }

  // Brief flash so pressing it gives feedback even with no station in range to ack —
  // this is fire-and-forget, there is no confirmation the station received it.
  // The 80 ms also lets the frame leave the radio before any sleep that follows.
  blink(1);
}

static bool startRadio() {
  WiFi.mode(WIFI_STA);

  // Pin a fixed channel rather than trusting whatever the radio lands on by
  // default — must match the station's remote_stop.cpp, which pins the same channel.
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  // C3 Super Mini boards are widely reported to have a poor antenna match that
  // breaks the radio at full TX power. Confirmed here 2026-09-14: with the default
  // power the station never heard a packet; with this cap the round trip works.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[setup] esp_now_init failed");
    return false;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_ADDR, 6);
  peer.channel = 1;     // must match the fixed channel set above
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[setup] esp_now_add_peer (broadcast) failed");
    return false;
  }
  return true;
}

static void goToSleep() {
  // A press still held would wake the board again at once and send a second STOP,
  // which would stop a second queued alarm — wait for the release first.
  while (digitalRead(BUTTON_PIN) == LOW) delay(10);
  delay(DEBOUNCE_MS);   // let the contact bounce settle before arming the wake-up

  Serial.println("[sleep] going to deep sleep");
  Serial.flush();
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);

  esp_deep_sleep_enable_gpio_wakeup(1ULL << BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();   // does not return; a wake-up runs setup() from the top
}

void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  ledSet(false);

  bool wokeByPress = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO);

  // Radio first and Serial after it: on a wake-up the press is what matters, and every
  // millisecond here is delay before the alarm stops (Report T19, target < 500 ms).
  bool radioOk = startRadio();

  if (wokeByPress) {
    if (radioOk) sendStopAlarm();
    lastPressMs = millis();
    lastButtonState = LOW;   // the waking press may still be held — not a new press
    awakeUntilMs = millis() + AWAKE_AFTER_PRESS_MS;
  } else {
    blink(2);                // powered up
    awakeUntilMs = millis() + AWAKE_AFTER_POWER_ON_MS;
  }

  Serial.begin(115200);
  Serial.println("=== ESP-NOW alarm-stop button (deep sleep) ===");
  Serial.println(wokeByPress ? "[setup] woken by the button" : "[setup] power-up");
  if (!radioOk) {
    Serial.println("[setup] radio failed - sleeping");
    goToSleep();
  }
  Serial.print("[setup] ready, this button's MAC ");
  Serial.print(WiFi.macAddress());
  Serial.print(", channel ");
  Serial.println(WiFi.channel());
}

void loop() {
  bool state = digitalRead(BUTTON_PIN);
  uint32_t now = millis();

  if (state == LOW && lastButtonState == HIGH && (now - lastPressMs) >= DEBOUNCE_MS) {
    lastPressMs = now;
    sendStopAlarm();
    awakeUntilMs = millis() + AWAKE_AFTER_PRESS_MS;
  }
  lastButtonState = state;

  if ((int32_t)(millis() - awakeUntilMs) >= 0) goToSleep();
}
