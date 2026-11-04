/*  Capacitive touch bring-up, step 2 of 2 — draws a dot where you touch.
    Section 8 / 12, touch half. Run Test_ctp_scan first; this assumes it passed.

    No WiFi, so safe on the faulty board. Wiring is the same as Test_ctp_scan.
    Display pins come from TFT_eSPI's own User_Setup.h, unchanged.

    GPIO 15 trap: User_Setup.h still has TOUCH_CS 15 (resistive touch), so tft.init()
    sets GPIO 15 as an output. CtpTouch::begin() is called AFTER tft.init() so Wire takes
    the pin back, and this sketch never calls tft.getTouch(), the only other thing that
    drives TOUCH_CS. So no User_Setup.h edit is needed for this test.
    If Test_ctp_scan shows ID MATCH but this sketch says "touch chip NOT found", that
    workaround failed — TOUCH_CS then has to come out of User_Setup.h (ask first: global
    toolchain file, and display.cpp's resistive getTouch() stops compiling without it).

    Build (hold BOOT, 115200, Serial Monitor closed):
      arduino-cli compile --fqbn "esp32:esp32:esp32:UploadSpeed=115200" \
        --library "D:/COOPProject/Coding/Firmware" --upload -p COM3 .

    HOW TO READ IT: tap inside each of the five rings. Aim for the middle, but you don't
    need to be exact. On release, Serial prints the tap's average position, the nearest
    ring, and the offset from that ring's centre.
      offsets within ~15 px, no pattern    -> auto calibration is fine, nothing to change
      offsets all in one direction/growing -> calibration needed
      dot lands on the ring                -> mapping correct, nothing to change
      left/right mirrored                  -> flip INVERT_X in src/ctp_touch.cpp
      top/bottom mirrored                  -> flip INVERT_Y
      finger moves across, dot moves down  -> flip SWAP_XY
      dot off by a constant few pixels     -> normal for a finger, not a fault
    No calibration step: unlike the XPT2046, the FT6336 reports pixels directly.
*/

#include <SPI.h>
#include <TFT_eSPI.h>

#include "ctp_touch.h"
#include "heater.h"

TFT_eSPI tft = TFT_eSPI();

// 40 px in from the edges so a fingertip-sized ring fits on screen.
static constexpr int16_t TARGETS[5][2] = {
  {40, 40}, {439, 40}, {40, 279}, {439, 279}, {240, 160}
};

// A ring about a fingertip wide, with a centre dot to aim at.
static void drawTarget(int16_t x, int16_t y) {
  tft.drawCircle(x, y, 20, TFT_WHITE);
  tft.drawCircle(x, y, 19, TFT_WHITE);
  tft.fillCircle(x, y, 3, TFT_YELLOW);
}

static void drawTargets() {
  tft.fillScreen(TFT_BLACK);
  for (const auto& t : TARGETS) drawTarget(t[0], t[1]);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawCentreString("Tap inside each ring. Hold 2 s top-left to clear.", 240, 215, 2);
}

void setup() {
  Heater::begin();   // SSR pin LOW first, every sketch (hard rule 3)

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== capacitive touch draw test (no WiFi) ===");

  tft.init();
  tft.setRotation(1);   // the only rotation CtpTouch::read() maps
  // The capacitive panel needs inversion OFF: Test_colors on it (2026-09-15) showed
  // every colour as its exact complement, because User_Setup.h has TFT_INVERSION_ON
  // for the RESISTIVE panel. Undone here rather than in User_Setup.h so the resistive
  // sketches keep working unchanged.
  tft.invertDisplay(false);
  drawTargets();

  if (CtpTouch::begin()) {   // after tft.init() — see GPIO 15 note above
    Serial.println("touch chip found");
  } else {
    Serial.println("touch chip NOT found - see header comment");
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawCentreString("TOUCH CHIP NOT FOUND", 240, 120, 4);
  }
}

// On release: average every sample of the tap, find the nearest ring, print the offset.
static void reportTap(int32_t sumX, int32_t sumY, uint16_t n) {
  int16_t ax = sumX / n, ay = sumY / n;
  uint8_t best = 0;
  int32_t bestD2 = INT32_MAX;
  for (uint8_t i = 0; i < 5; i++) {
    int32_t dx = ax - TARGETS[i][0], dy = ay - TARGETS[i][1];
    if (dx * dx + dy * dy < bestD2) { bestD2 = dx * dx + dy * dy; best = i; }
  }
  int16_t dx = ax - TARGETS[best][0], dy = ay - TARGETS[best][1];

  tft.fillCircle(ax, ay, 4, TFT_RED);   // averaged position, over the live green dots

  char line[64];
  snprintf(line, sizeof(line), "ring (%3d,%3d)  tap (%3d,%3d)  off dx=%+4d dy=%+4d",
           TARGETS[best][0], TARGETS[best][1], ax, ay, dx, dy);
  Serial.println(line);
  snprintf(line, sizeof(line), "off dx=%+d dy=%+d", dx, dy);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextPadding(200);
  tft.drawCentreString(line, 240, 100, 2);
  tft.setTextPadding(0);
}

void loop() {
  static uint32_t cornerSince = 0;
  static int32_t sumX = 0, sumY = 0;
  static uint16_t samples = 0;

  int16_t x = 0, y = 0;
  if (!CtpTouch::read(x, y)) {
    if (samples > 0) reportTap(sumX, sumY, samples);
    sumX = sumY = 0;
    samples = 0;
    cornerSince = 0;
    delay(20);
    return;
  }

  tft.fillCircle(x, y, 2, TFT_GREEN);
  if (samples < 60000) { sumX += x; sumY += y; samples++; }

  // Held in the top-left corner for 2 s -> clear the dots.
  if (x < 60 && y < 60) {
    if (cornerSince == 0) cornerSince = millis();
    if (millis() - cornerSince >= 2000) {
      drawTargets();
      cornerSince = 0;
    }
  } else {
    cornerSince = 0;
  }

  delay(20);
}
