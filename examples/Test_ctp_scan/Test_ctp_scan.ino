/*  Capacitive touch bring-up, step 1 of 2 — Serial only (115200), no drawing.
    Section 8 / 12, touch half. Driver: src/ctp_touch.*

    No WiFi anywhere in this sketch, so the radio never starts — safe on the faulty
    board, which only browns out at radio start.

    Deliberately does NOT use TFT_eSPI: with TOUCH_CS 15 still in User_Setup.h,
    tft.init() would drive GPIO 15 as an output. Leaving the display out means a result
    here is about the touch wiring alone.

    WIRING (capacitive pins only — display pins unchanged, VCC 5V, LED 3V3 per Rev 9):
      CTP_SDA -> GPIO 15
      CTP_SCL -> GPIO 22
      CTP_INT -> GPIO 36 (terminal SP)   optional, only watched, never needed
      CTP_RST -> 3V3                     NOT a GPIO, and NOT joined to LCD RESET
    The vendor's ESP32 demo wires CTP pins straight to the ESP32 with VCC on 5V, which
    points to 3.3V logic on this header. ACTION NEEDED: confirm on the board itself.

    Build (hold BOOT, 115200, Serial Monitor closed):
      arduino-cli compile --fqbn "esp32:esp32:esp32:UploadSpeed=115200" \
        --library "D:/COOPProject/Coding/Firmware" --upload -p COM3 .

    READ THE RESULT, top to bottom:
      [1] idle levels   SDA/SCL HIGH  -> pull-ups present (module has its own)
                        either LOW    -> no pull-up, short to GND, or wire off.
                                         Add 4.7k to 3V3 on that line and re-run.
      [2] bus scan      "0x38"        -> chip answers. Good.
                        nothing found -> power, SDA/SCL swapped, or GPIO 22 (known
                                         suspect on this board). Swap the two wires once.
                        other address -> not the FT6336 — record it before changing code.
      [3] chip ID       "ID MATCH"    -> it is an FT6336. Go on to Test_ctp_touch.
      [4] touch         raw x 0-319, raw y 0-479 while touching, "released" on lift.
                        INT level should go LOW while touching. If coordinates work but
                        INT never moves, INT is not needed — just record it.
*/

#include <Wire.h>

#include "ctp_touch.h"
#include "heater.h"

static void printIdleLevels() {
  pinMode(CtpTouch::PIN_SDA, INPUT);
  pinMode(CtpTouch::PIN_SCL, INPUT);
  pinMode(CtpTouch::PIN_INT, INPUT);
  delay(10);
  Serial.printf("[1] idle levels: SDA(%u)=%s  SCL(%u)=%s  INT(%u)=%s\n",
                CtpTouch::PIN_SDA, digitalRead(CtpTouch::PIN_SDA) ? "HIGH" : "LOW",
                CtpTouch::PIN_SCL, digitalRead(CtpTouch::PIN_SCL) ? "HIGH" : "LOW",
                CtpTouch::PIN_INT, digitalRead(CtpTouch::PIN_INT) ? "HIGH" : "LOW");
  // A floating pin can read HIGH by chance, so HIGH is not proof — LOW is a clear fail.
}

static void scanBus() {
  Serial.println("[2] I2C scan:");
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("    device at 0x%02X%s\n", addr,
                    addr == CtpTouch::I2C_ADDR ? "  <- FT6336" : "");
      found++;
    }
  }
  if (found == 0) Serial.println("    nothing found");
}

static void printChipId() {
  CtpTouch::ChipInfo info;
  CtpTouch::identify(info);
  Serial.printf("[3] chip ID: A8=0x%02X (0x11)  9F=0x%02X (0x26)  A0=0x%02X (00-02)  A3=0x%02X (0x64)\n",
                info.focaltechId, info.cipherMid, info.cipherLow, info.cipherHigh);
  if (!info.responded)   Serial.println("    NO RESPONSE to ID reads");
  else if (info.matches) Serial.printf("    ID MATCH (%s)\n",
                                       info.cipherLow == 0x02 ? "FT6336U" :
                                       info.cipherLow == 0x01 ? "FT6336G" : "FT6336");
  else                   Serial.println("    responded, but ID does NOT match an FT6336");
}

void setup() {
  Heater::begin();   // SSR pin LOW first, every sketch (hard rule 3)

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== capacitive touch scan (no WiFi, no display) ===");

  printIdleLevels();      // must run before Wire takes the pins
  CtpTouch::begin();      // result printed in detail below
  scanBus();
  printChipId();

  Serial.println("[4] touch the panel...");
}

void loop() {
  static bool wasTouched = false;
  static uint32_t lastPrint = 0;

  uint16_t rx = 0, ry = 0;
  bool touched = CtpTouch::readRaw(rx, ry);

  if (touched && millis() - lastPrint >= 100) {
    lastPrint = millis();
    int16_t sx = 0, sy = 0;
    CtpTouch::read(sx, sy);
    Serial.printf("raw x=%3u y=%3u  ->  screen X=%3d Y=%3d  INT=%s\n", rx, ry, sx, sy,
                  digitalRead(CtpTouch::PIN_INT) ? "HIGH" : "LOW");
  }
  if (!touched && wasTouched) Serial.println("released");
  wasTouched = touched;

  delay(20);
}
