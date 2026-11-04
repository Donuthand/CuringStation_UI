// See ctp_touch.h for which spec sections this implements.
//
// Register addresses and ID values come from the vendor's own ESP32 driver in the
// module's download package (capactive.zip: 1-Demo/Demo_ESP32/Install libraries/
// FT6336-arduino). That library is deliberately not installed: it pulses CTP_RST from a
// GPIO (ours is tied to 3V3), prints and carries on when the chip is missing, and never
// checks whether an I2C read was ACKed — a bus fault would read as 0xFF "coordinates".

#include "ctp_touch.h"
#include <Wire.h>

namespace CtpTouch {

static constexpr uint8_t REG_TD_STATUS    = 0x02;  // low nibble = number of touch points
static constexpr uint8_t REG_P1_XH        = 0x03;  // P1 XH, XL, YH, YL
static constexpr uint8_t REG_CIPHER_MID   = 0x9F;  // followed by CIPHER_LOW at 0xA0
static constexpr uint8_t REG_CIPHER_HIGH  = 0xA3;
static constexpr uint8_t REG_FOCALTECH_ID = 0xA8;

// 100 kHz, not the chip's 400 kHz: dupont wires on the bench, and a touch poll is a
// handful of bytes, so speed buys nothing.
static constexpr uint32_t I2C_FREQ = 100000;

// Vendor driver waits 500 ms after its reset pulse. Ours resets at power-up, which
// happens with the ESP32's own boot, so a shorter wait from begin() is enough.
// ASSUMPTION — if the first ID read fails but a later one works, raise this.
static constexpr uint32_t POWER_UP_MS = 300;

static constexpr uint16_t PANEL_SHORT = 320;  // native portrait width
static constexpr uint16_t PANEL_LONG  = 480;

// Raw -> rotation 1. ASSUMPTION: taken from the vendor driver's ROTATION_RIGHT case
// (screen X = raw Y, screen Y = 320 - raw X), not yet seen on this unit.
// Test_ctp_touch proves it; fix a wrong result here, nowhere else.
static constexpr bool SWAP_XY  = true;
static constexpr bool INVERT_X = false;
static constexpr bool INVERT_Y = true;

static bool readReg(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return false;  // NACK: nothing at 0x38, or bus stuck
  if (Wire.requestFrom(I2C_ADDR, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

bool begin() {
  Wire.begin(PIN_SDA, PIN_SCL, I2C_FREQ);
  delay(POWER_UP_MS);
  ChipInfo info;
  return identify(info);
}

bool identify(ChipInfo& info) {
  info = ChipInfo{};
  uint8_t mid[2] = {0, 0};
  info.responded = readReg(REG_FOCALTECH_ID, &info.focaltechId, 1)
                && readReg(REG_CIPHER_MID, mid, 2)
                && readReg(REG_CIPHER_HIGH, &info.cipherHigh, 1);
  info.cipherMid = mid[0];
  info.cipherLow = mid[1];
  info.matches = info.responded
              && info.focaltechId == 0x11
              && info.cipherMid == 0x26
              && info.cipherLow <= 0x02
              && info.cipherHigh == 0x64;
  return info.matches;
}

bool readRaw(uint16_t& x, uint16_t& y) {
  uint8_t status;
  if (!readReg(REG_TD_STATUS, &status, 1)) return false;
  uint8_t touches = status & 0x0F;
  if (touches == 0 || touches > 2) return false;  // FT6336 tracks at most 2 points

  uint8_t d[4];
  if (!readReg(REG_P1_XH, d, 4)) return false;
  // Event flag, top two bits of XH: 0 press, 1 lift, 2 contact, 3 NO event — the chip
  // saying these coordinates are not a touch at all.
  if ((d[0] >> 6) == 3) return false;
  x = ((uint16_t)(d[0] & 0x0F) << 8) | d[1];
  y = ((uint16_t)(d[2] & 0x0F) << 8) | d[3];
  return true;
}

bool read(int16_t& x, int16_t& y) {
  uint16_t rx, ry;
  if (!readRaw(rx, ry)) return false;

  uint16_t sx = SWAP_XY ? ry : rx;
  uint16_t sy = SWAP_XY ? rx : ry;
  // A point off the panel is a bad reading, not a touch at the edge. It used to be CLAMPED,
  // and with SWAP_XY + INVERT_Y a garbage 0xFFF pair clamps to (479, 0) — the top-right
  // corner, which on UIFuji's Home is the heating area: the heat off timer kept opening by
  // itself (Dough, 2026-09-23). Rejected instead, and logged (at most once a second) so the
  // raw values can be seen in the Serial Monitor.
  if (sx > PANEL_LONG - 1 || sy > PANEL_SHORT - 1) {
    static uint32_t lastLogMs = 0;
    if (millis() - lastLogMs >= 1000) {
      lastLogMs = millis();
      Serial.printf("[touch] rejected off-panel point raw x=%u y=%u\n", rx, ry);
    }
    return false;
  }
  if (INVERT_X) sx = PANEL_LONG - 1 - sx;
  if (INVERT_Y) sy = PANEL_SHORT - 1 - sy;

  x = (int16_t)sx;
  y = (int16_t)sy;
  return true;
}

} // namespace CtpTouch
