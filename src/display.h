// TFT 4" (ILI9488) + XPT2046 touch — implements Section 14 (UI Design) and the
// touch half of Section 12 (Hybrid Input) of epoxy_curing_station_full.md.
//
// ACTION NEEDED (Section 18): before this compiles/runs correctly, TFT_eSPI's
// User_Setup.h must be edited to match the Rev 5 pin table. See
// Coding/Firmware/User_Setup_epoxy.h in this folder for the exact block to paste in —
// TFT_eSPI reads pins from its own library config, never from sketch code.
#pragma once

#include <Arduino.h>

// Slot visual state, per Section 14 "Slot states" table.
enum class SlotState : uint8_t {
  Empty,   // Dim border, "Empty — tap Start"
  Active,  // Green border, progress bar shown
  Done     // Red border, "DONE"
};

struct SlotView {
  SlotState state = SlotState::Empty;
  char initials[3] = "";   // 2-letter avatar, e.g. "DG"
  char name[16] = "";      // e.g. "Dough"
  uint16_t totalMinutes = 0;
  uint8_t setpointC = 0;
  uint32_t remainingSeconds = 0; // counts down to 0
};

// Header state, per Section 14 main screen layout: "[HEAT ON] 45.2°C".
struct HeaderView {
  bool heatOn = false;
  float currentTempC = 0.0f;
};

namespace Display {

// Initializes TFT_eSPI and XPT2046_Touchscreen (Touch CS GPIO15, Touch IRQ GPIO36
// per Section 9). Call once from setup().
void begin();

// Redraws the full main screen: header bar + 6 slot rows + bottom button bar,
// per Section 14's main screen layout diagram.
void drawMainScreen(const HeaderView& header, const SlotView slots[6]);

// Polls the touch controller. Returns true and fills x,y (screen pixel coords)
// if a touch is currently registered, false otherwise. Section 12: touch is used
// for QWERTY name entry and list taps; numeric entry stays on the keypad.
bool readTouch(int16_t& x, int16_t& y);

// Progress-bar color per Section 14 "Progress bar color" table (>60% green,
// 25-60% orange, <25% red). remainingFraction is 0.0-1.0.
uint16_t progressBarColor(float remainingFraction);

// Slot border color per Section 14 "Slot states" table.
uint16_t slotBorderColor(SlotState state);

} // namespace Display
