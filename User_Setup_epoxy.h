// Reference block for TFT_eSPI's User_Setup.h — NOT compiled directly.
// TFT_eSPI reads pins only from its own library config header, never from sketch
// code (Section 18 ACTION NEEDED). Copy the driver + pin section below into
// <Arduino/libraries/TFT_eSPI/User_Setup.h> (or select/create a matching User_Setup
// and point User_Setup_Select.h at it) before building any sketch that uses display.h.
//
// On this machine that file is:
//   C:\Users\phatc\OneDrive\Documents\Arduino\libraries\TFT_eSPI\User_Setup.h
// and it has already been edited to match this block (2026-07-28/29).
//
// Pins below are the Rev 10 SPI bus (spec Rev 14, rollback section):
//   MOSI 23 / MISO 19 / SCK 18 / CS 5 / DC 2 / RST 13, touch CS 15
// The 2026-08-23 Rev 11 reshuffle (MOSI 5 / CS 13 / RST 15 / touch CS 23) did not
// work on hardware and was rolled back on 2026-08-24; do not reapply it.
//
// This block is NOT compiled. TFT_eSPI's own User_Setup.h must hold the same
// #defines (re-written from this file 2026-10-26 after the laptop wipe).

// FACT: the purchased 4" 480x320 module is ST7796-driven (seller listing,
// cybertice.com product 3725), not the ILI9488 the spec originally assumed.
// ACTION NEEDED: update epoxy_curing_station_full.md Sections 9/18 to match.
#define ST7796_DRIVER

#define TFT_MOSI  23
#define TFT_MISO 19
#define TFT_SCLK 18
#define TFT_CS   5

// DC is GPIO2, per Section 9. Do not "fix" this to GPIO22 — that was tried on
// 2026-07-29 (config only, wire never moved), left the panel white on 2026-07-30, and
// was reverted. GPIO2 is confirmed working.
//
// Known tradeoff, accepted: GPIO2 is a boot strapping pin, so uploads need the
// BOOT/IO0 button held while esptool connects, or they fail with "Wrong boot mode
// detected (0x13)". Moving DC to GPIO22 would remove that, but only as a wire move
// plus config change together, verified on hardware.
#define TFT_DC    2

// GPIO15 is a strapping pin (MTDO) with an internal pull-up, so it sits HIGH through
// boot and TFT_eSPI only pulls it low after boot, during begin(). Accepted. Side
// effect: holding GPIO15 low at reset silences the ROM boot log on U0TXD.
#define TFT_RST  13

// Display and touch share one SPI bus with separate chip selects: T_CLK/T_DIN/T_DO
// are jumpered to the same ESP32 pins as SCK/SDI/SDO (18/5/19), with only the chip
// selects separate. T_DIN has no pin of its own — it is a wire back to GPIO5
// alongside MOSI. Giving it a separate pin is not a config option; TFT_eSPI drives
// touch on TFT_MOSI/TFT_MISO/TFT_SCLK and reads only TOUCH_CS from here. Defining TOUCH_CS enables TFT_eSPI's built-in touch, which
// arbitrates that shared bus. Do NOT additionally use the standalone
// XPT2046_Touchscreen library on these pins.
//
// CORRECTION (2026-07-30): earlier comments here claimed the MODULE ties the touch
// pins to the display SPI internally. That was never verified and the 14-pin header
// argues against it — T_DO/T_DIN/T_CLK are separate physical pins from SDO/SCK/SDI.
// They are shared because that is how WE wired them. The real constraint is the pin
// budget: giving touch its own bus needs 3 spare OUTPUT pins and there are not 3
// free (GPIO34/36/39 are input-only; GPIO0/12 are strapping pins), so the shared bus
// stays.
//
// T_IRQ IS wired, to GPIO36 (breakout terminal "SVP"), per Section 9. TFT_eSPI has no
// interrupt hook, so sketches read it with a plain digitalRead as a gate before
// touching the SPI bus — see Coding/Test_touch. Nothing to configure here.
// GPIO23 chosen 2026-08-23, freed by moving MOSI to GPIO5. It had to be an output,
// which rules out 34/36/39, and GPIO0 (BOOT) and GPIO12 (MTDI, must be LOW at reset)
// are not safe to load. That left GPIO22 and GPIO23.
//
// GPIO22 was passed over because it has an UNEXPLAINED HARDWARE FAILURE against it:
// the TFT backlight gate was wired to it and driven from firmware on 2026-08-21 and
// did not work (see src/ui.h and Project_Status.md). Nothing in the datasheet
// predicts that — 22 is not a strapping pin, not flash-reserved, not UART0 — so the
// cause is still open. Do not move a signal onto GPIO22 until a bare blink test has
// cleared it. This is NOT the same as the 2026-07-30 "DC does not work on GPIO22"
// claim, which was a config-only edit with the wire never moved and proves nothing.
#define TOUCH_CS 15

// FACT: 27 MHz, verified clean on this hardware 2026-07-31 (raised from the 5 MHz used while
// debugging the blank screen). Deliberately not pushed to 40 — 27 already repaints the full
// panel in ~0.09s against a 1-second countdown tick, so faster buys nothing visible while
// stressing dupont-wire signal integrity for no reason.
//
// SPI_TOUCH_FREQUENCY must stay at 2.5 MHz — that is an XPT2046 limit, and raising it
// would invalidate the touch calibration in display.cpp.
#define SPI_FREQUENCY       27000000
// DIAGNOSTIC 2026-08-24: dropped from 27 MHz. After the Rev 11 rewire the panel showed
// magenta corruption — a full-height vertical stripe and scattered dithered blocks, in a
// colour the UI palette does not contain, so corrupted SPI data rather than a drawing bug.
// 27 MHz was verified clean on 2026-07-31 but against the OLD wire routing; the board is now
// on breadboard + dupont jumpers. If 10 MHz is clean, walk back up (16, 20) to find the real
// margin and record it. SPI_READ_FREQUENCY left at 20 MHz deliberately — one variable at a time.
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000

// FACT: the working User_Setup.h uses the HSPI peripheral, not the VSPI default.
// None of 18/5/19 are HSPI's default pins (nor, since MOSI moved off 23, all of
// VSPI's) but TFT_eSPI drives them explicitly, so this works. It is not optional —
// leave it out and the pins move.
#define USE_HSPI_PORT

// Font set matches the live User_Setup.h. Only fonts 1/2/4 are used by display.cpp
// today; the rest are loaded and cost flash, which is fine at 24% usage.
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// Rev 13 (2026-09-25): OFF. The station's screen is now the capacitive module (ST7796S +
// FT6336U, LCDWIKI MSP4030/4031), which shows every colour as its exact complement WITH
// inversion on — the opposite of the old resistive panel. Until TFT_eSPI's own User_Setup.h
// gets the same change, Test_ui_fuji's tft.invertDisplay(false) after init() covers it.
// History: the RESISTIVE panel REQUIRED TFT_INVERSION_ON (confirmed 2026-07-29, one of the
// three stacked faults behind the blank-screen session). Test_ui still targets that panel, so
// with this line off it will show inverted colours — expected, not a fault.
// #define TFT_INVERSION_ON
