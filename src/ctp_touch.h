// FT6336U capacitive touch (LCDWIKI MSP4030/MSP4031 module) over I2C — a CANDIDATE for
// the touch half of Section 8 (Controller & Input Hardware) and Section 12 (Hybrid Input).
//
// SEPARATE from the resistive path, on purpose: display.cpp, Test_ui, Test_touch and the
// XPT2046 calibration are untouched and still use TFT_eSPI's built-in touch. Nothing in
// the station firmware includes this file. If the capacitive module is rejected, delete
// ctp_touch.* and the two Test_ctp_* examples and nothing else changes.
//
// ACTION NEEDED: only if the capacitive module is adopted — wire it into display.cpp and
// move Sections 8/9/12/18 to this part in the batched Rev 13.
//
// Bring-up order, same idea as the resistive screen (Test_colors -> Test_touch):
//   1. examples/Test_ctp_scan   — Serial only, no TFT_eSPI: wiring, I2C, chip ID, raw X/Y
//   2. examples/Test_ctp_touch  — draws where you touch, proves the rotation mapping
//
// Neither sketch starts WiFi, so both are safe on the faulty board (it only browns out
// when the radio starts — Project_Status 2026-09-15).
#pragma once

#include <Arduino.h>

namespace CtpTouch {

// Pins — ASSUMPTION, from the capacitive-swap GPIO budget, not yet in Section 9.
// Removing the XPT2046 frees 15 (was TOUCH_CS) and 36 (was T_IRQ). SCL has to be an
// output-capable pin and the only other free one is 22.
// GPIO 22 carries an UNEXPLAINED failure on the station board (2026-08-21 backlight
// gate). If Test_ctp_scan finds nothing on the bus, suspect this pin early.
constexpr uint8_t PIN_SDA = 15;
constexpr uint8_t PIN_SCL = 22;
// CTP_INT, input-only pin (terminal SP/SVP) — the same terminal the XPT2046's T_IRQ used,
// so it is a known-good terminal on this board. Nothing here needs it: read() polls. It
// is wired so Test_ctp_scan can show whether the line moves when touched.
constexpr uint8_t PIN_INT = 36;
// CTP_RST is NOT a GPIO: tied straight to 3V3, the chip resets itself at power-up.
// It is a separate net from the LCD RESET (MSP4031 schematic) — do not join them.

constexpr uint8_t I2C_ADDR = 0x38;          // vendor driver FT6336_ADDR

// ID registers as the vendor's own driver checks them.
struct ChipInfo {
  bool responded = false;   // all ID reads got an ACK
  bool matches = false;     // values are what an FT6336 should report
  uint8_t focaltechId = 0;  // reg 0xA8, expect 0x11
  uint8_t cipherMid = 0;    // reg 0x9F, expect 0x26
  uint8_t cipherLow = 0;    // reg 0xA0, 0x00-0x02 (0x01 FT6336G, 0x02 FT6336U)
  uint8_t cipherHigh = 0;   // reg 0xA3, expect 0x64
};

// Starts I2C on PIN_SDA/PIN_SCL and checks the chip ID. Returns true if an FT6336
// answered. If TFT_eSPI is in the same sketch, call this AFTER tft.init(): with
// TOUCH_CS 15 still in User_Setup.h, init() claims GPIO 15 as an output, and this call
// is what hands the pin back to I2C.
bool begin();

// Re-reads the ID registers. Safe to call any time after begin().
bool identify(ChipInfo& info);

// Panel-native coordinates (portrait, X 0-319, Y 0-479). False = no touch or I2C error.
bool readRaw(uint16_t& x, uint16_t& y);

// Screen coordinates for tft.setRotation(1), the landscape the whole UI uses (480x320).
// Valid for rotation 1 ONLY, same limit as the resistive calibration in display.cpp.
bool read(int16_t& x, int16_t& y);

} // namespace CtpTouch
