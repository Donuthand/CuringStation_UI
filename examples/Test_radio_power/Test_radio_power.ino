/*  Brownout isolation test for the station board: does starting the WiFi radio on its
    own brown out this board? No UI, no DFPlayer, no keypad, no ESP-NOW — only the SSR
    held off, Serial, and WiFi.mode(WIFI_STA), which is the step Test_ui's
    RemoteStop::begin() runs.

    Build (station board, same quirks as Test_ui — hold BOOT, 115200, monitor closed):
      arduino-cli compile --fqbn "esp32:esp32:esp32:UploadSpeed=115200" \
        --library "D:/COOPProject/Coding/Firmware" --upload -p COM3 .

    Read the result:
      "radio up" then "alive Ns" counting  -> radio alone is fine; the load is elsewhere
      "E BOD: Brownout detector" after "starting radio" -> the radio start alone is
        enough to brown out this board as it is wired now
*/

#include <WiFi.h>

#include "heater.h"

void setup() {
  Heater::begin();   // SSR pin LOW first, same as Test_ui

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== radio power test ===");
  Serial.println("starting radio...");
  Serial.flush();    // a brownout reset drops anything still queued, so push it out now

  WiFi.mode(WIFI_STA);
  delay(500);

  Serial.println("radio up - no brownout");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last >= 1000) {
    last = millis();
    Serial.printf("alive %lus\n", (unsigned long)(last / 1000));
  }
}
