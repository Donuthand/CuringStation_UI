// Alarm audio — DFPlayer Mini on Serial2. Implements Section 11's audio alarm and
// the sound half of Section 19; step F of the UI build.
//
// Pins (Section 9/10, Rev 5): GPIO17 = Serial2 TX -> 1 kOhm -> DFPlayer RX,
// GPIO35 = Serial2 RX <- DFPlayer TX, direct. The Rev 2.0 sheet's GPIO 1/3 would
// collide with UART0 and break the Serial Monitor, which is the whole reason those
// moved. GPIO35 is input-only, which is fine because this direction is receive-only.
//
// Tracks are 0001.mp3 .. 0010.mp3 on a FAT32 microSD, in the order of
// Station::SOUND_NAMES, so a Station sound index of 0..9 is simply track index + 1.
// A missing file plays nothing — copy all ten onto the card.
// Section 19's "swap the MP3 on the card, no code change" depends on that mapping
// staying positional — do not sort or rename the files.
//
// WIRING TRAP, same shape as the TFT header's LED pin: on the module's pin side the
// order is VCC, RX, TX, DAC_R, DAC_L, SPK_2, GND, SPK_1 — so **GND sits between the
// two speaker pins**. The speaker straddles SPK_2 and SPK_1 with a gap, not two
// adjacent holes. Wiring two adjacent pins lands on DAC_L/DAC_R, which are line-level
// with no amp behind them: audible but very quiet, which reads as a weak speaker
// rather than a wiring error. Read the silkscreen, do not count pins.
//
// Holds no timer or queue logic: it is told what to play and for how long. Deciding
// WHEN an alarm sounds is the sketch's job, the same split as TempSensor feeding
// UI::setHeader().
#pragma once

#include <Arduino.h>

namespace Sound {

// HARMLESS, do not chase: GPIO35 is input-only, so Serial2.begin() logs
//   E (xxx) gpio: gpio_pullup_en(85): GPIO number error (input-only pad has no internal PU)
// once at boot. The driver is refusing an internal pull-up the pad does not have; RX still
// works and the DFPlayer comes up online. Verified 2026-08-25 while debugging a white panel,
// where this line was a red herring.
constexpr uint8_t  PIN_RX = 35;   // ESP32 receives here; DFPlayer TX
constexpr uint8_t  PIN_TX = 17;   // ESP32 transmits here; 1 kOhm to DFPlayer RX
constexpr uint32_t BAUD   = 9600; // the DFPlayer Mini is fixed at 9600

constexpr uint8_t TRACK_COUNT    = 10;  // matches Station::SOUND_COUNT
constexpr uint8_t VOLUME_MAX     = 30;  // the DFPlayer's own scale is 0-30
// Back to 22, the original value, 2026-08-04, and again 2026-08-18. Everything tried in
// between made it quieter in Test_ui, not louder. The 2026-08-18 attempt raised this to
// the 30 ceiling AND re-sent it in play(), which was the one untried combination — it was
// quieter still. See the volume section of Project_Status.md before touching this.
// 22 -> 20 on 2026-09-25: the station's volume setting is now capped at 20 to save
// current, and this has to equal Station::ALARM_VOL_DEFAULT (static_assert in Test_ui_fuji).
constexpr uint8_t VOLUME_DEFAULT = 20;

// Opens Serial2 and resets the module. Returns false if nothing reported a card
// online — check the SD card and its file names first, then the 1 kOhm on RX, then
// that VCC really is on 5V and not 3.3V.
//
// Takes up to ~2.2 s because of the reset handshake, so call it before the UI is
// showing anything that has to stay live. A false here is no longer final (2026-10-16):
// poll() keeps resetting the module every 5 s and goes online on a late "card online".
bool begin();

// Call every loop. Stops playback once the requested duration has elapsed. Cheap
// when nothing is sounding. If the module restarts mid-tone it re-sends the volume and
// starts the tone again (blocks ~100 ms, only on that rare event).
void poll();

// Repeats `soundIndex` (a Station sound index, 0..TRACK_COUNT-1) for `seconds`,
// then stops on its own from poll(). Repeats rather than plays once because the
// tones are short and the alarm has to last Station::alarmDurationSec().
void play(uint8_t soundIndex, uint8_t seconds);

// Immediate silence, for when an alarm is acknowledged.
void stop();

bool playing();

// True once the module has answered, at begin() or later from poll(). Everything else is
// a no-op when false, so a missing DFPlayer costs the alarm its noise and nothing else.
bool online();

void setVolume(uint8_t v);   // clamped to 0..VOLUME_MAX

// How many times poll() has seen the module restart ("card online" / "card inserted").
// For tests that run off a charger with no Serial Monitor (Test_full_load).
uint32_t restartCount();

} // namespace Sound
