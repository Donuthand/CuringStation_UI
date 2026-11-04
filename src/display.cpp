// See display.h for which spec sections this implements.
//
// Touch is handled by TFT_eSPI's own built-in XPT2046 support, NOT the standalone
// XPT2046_Touchscreen library. The module's touch pins (T_CLK/T_DIN/T_DO) are
// separate pins on the 14-pin header — they are *wired* to the display's SPI lines
// (23/19/18) here, not tied together inside the module. That was a deliberate
// choice: a separate touch bus needs three spare output pins and only GPIO2 is
// free, since GPIO34/36/39 are input-only. All touch pins live in User_Setup.h
// (TOUCH_CS 15) per hard rule 4 — nothing to set here. T_IRQ IS wired, on GPIO36
// (terminal SVP) per Section 9; TFT_eSPI has no interrupt hook, so it can only be
// used as a plain digitalRead gate — see Coding/Test_touch/.

#include "display.h"
#include <TFT_eSPI.h>
#include <SPI.h>

static TFT_eSPI tft;

// Layout constants for the 480x320 ST7796 panel in landscape, following Section 14's
// main screen diagram (header bar, 6 stacked slot rows, bottom button bar).
static constexpr int16_t SCREEN_W = 480;
static constexpr int16_t SCREEN_H = 320;
static constexpr int16_t HEADER_H = 32;
static constexpr int16_t FOOTER_H = 40;
static constexpr int16_t SLOT_COUNT = 6;
static constexpr int16_t SLOT_H = (SCREEN_H - HEADER_H - FOOTER_H) / SLOT_COUNT;

namespace Display {

// Measured on this unit 2026-07-31 with Coding/Test_touch_calibrate/ (four-corner
// calibrateTouch run). Valid for setRotation(1) ONLY — the last value encodes the
// orientation, so changing the rotation below invalidates all five and the
// calibration has to be re-run.
static uint16_t touchCalibration[5] = { 182, 3586, 349, 3346, 4 };

void begin() {
  tft.init();
  tft.setRotation(1); // landscape, matches Section 14's wide layout diagram
  tft.setTouch(touchCalibration);
  tft.fillScreen(TFT_BLACK);
}

bool readTouch(int16_t& x, int16_t& y) {
  // getTouch returns screen pixel coords directly, already mapped via setTouch above.
  uint16_t tx = 0, ty = 0;
  if (!tft.getTouch(&tx, &ty)) return false;
  x = (int16_t)tx;
  y = (int16_t)ty;
  return true;
}

uint16_t progressBarColor(float remainingFraction) {
  if (remainingFraction > 0.60f) return TFT_GREEN;
  if (remainingFraction >= 0.25f) return TFT_ORANGE;
  return TFT_RED;
}

uint16_t slotBorderColor(SlotState state) {
  switch (state) {
    case SlotState::Active: return TFT_GREEN;
    case SlotState::Done:   return TFT_RED;
    case SlotState::Empty:
    default:                return TFT_DARKGREY;
  }
}

static void drawHeader(const HeaderView& h) {
  tft.fillRect(0, 0, SCREEN_W, HEADER_H, TFT_NAVY);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextSize(1);
  tft.drawString("EPOXY CURE", 8, 10, 2);

  // Degree symbol omitted — TFT_eSPI's built-in fonts don't reliably include it;
  // ACTION NEEDED: confirm glyph support in the chosen font before adding one.
  char status[24];
  if (h.heatOn) {
    snprintf(status, sizeof(status), "[HEAT ON] %.1fC", h.currentTempC);
  } else {
    snprintf(status, sizeof(status), "%.1fC", h.currentTempC);
  }
  tft.setTextColor(h.heatOn ? TFT_RED : TFT_WHITE, TFT_NAVY);
  tft.drawRightString(status, SCREEN_W - 8, 10, 2);
}

static void drawSlot(int index, const SlotView& s) {
  int16_t y = HEADER_H + index * SLOT_H;
  tft.fillRect(0, y, SCREEN_W, SLOT_H, TFT_BLACK);
  tft.drawRect(2, y + 2, SCREEN_W - 4, SLOT_H - 4, slotBorderColor(s.state));

  char line[48];
  int16_t textY = y + 6;

  if (s.state == SlotState::Empty) {
    snprintf(line, sizeof(line), "%d      Empty - tap Start", index + 1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(line, 8, textY, 2);
    return;
  }

  uint32_t mm = s.remainingSeconds / 60;
  uint32_t ss = s.remainingSeconds % 60;
  if (s.state == SlotState::Done) {
    snprintf(line, sizeof(line), "%d [%s] %-10s %3u min - %uC  DONE",
             index + 1, s.initials, s.name, s.totalMinutes, s.setpointC);
  } else {
    snprintf(line, sizeof(line), "%d [%s] %-10s %3u min - %uC  %02u:%02u",
             index + 1, s.initials, s.name, s.totalMinutes, s.setpointC, mm, ss);
  }
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(line, 8, textY, 2);

  if (s.state == SlotState::Active && s.totalMinutes > 0) {
    float remainingFraction = (float)s.remainingSeconds / ((float)s.totalMinutes * 60.0f);
    int16_t barX = 40, barY = y + SLOT_H - 14, barW = SCREEN_W - 80, barH = 8;
    tft.drawRect(barX, barY, barW, barH, TFT_WHITE);
    int16_t fillW = (int16_t)(barW * remainingFraction);
    tft.fillRect(barX + 1, barY + 1, max(0, fillW - 2), barH - 2, progressBarColor(remainingFraction));
  }
}

static void drawFooter() {
  int16_t y = SCREEN_H - FOOTER_H;
  tft.fillRect(0, y, SCREEN_W, FOOTER_H, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawString("[Menu]", 20, y + 12, 2);
  tft.drawString("[+ Start]", SCREEN_W / 2 - 40, y + 12, 2);
  tft.drawString("[Clear done]", SCREEN_W - 140, y + 12, 2);
}

void drawMainScreen(const HeaderView& header, const SlotView slots[6]) {
  drawHeader(header);
  for (int i = 0; i < SLOT_COUNT; i++) {
    drawSlot(i, slots[i]);
  }
  drawFooter();
}

} // namespace Display
