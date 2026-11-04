// UIFuji — the Fujifilm UI for the CAPACITIVE module (ST7796S + FT6336U).
// A separate copy of ui.h/ui.cpp by decision (Project_Status 2026-09-15): UI / Test_ui
// keep driving the resistive panel in the old dark style, this drives the capacitive one
// from examples/Test_ui_fuji.
//
// Design authority since 2026-10-09: new_ui_try/5_fujifilm_concept_B_bigtext_try.html (Concept B
// with text x1.2, amber user names, no initials badge). Before that, 2026-09-23 →:
// new_ui_try/5_fujifilm_concept_B_2026-09-22.html +
// TFT_UI_HANDOFF.md (Concept B, dark, 6 fixed cards). Ported in place in two stages:
//   stage 1 (flashed 2026-09-23): Home — header with wordmark, 6 cards, footer — and the
//            green completion overlay; the heating area opens the heat off timer directly.
//   stage 2 (flashed and accepted 2026-09-23): every subpage, the optional Curing position step,
//            presets 60/75/90/105, Settings row 5 straight to the timer, temp-log gaps,
//            countdowns held while the power-loss page waits.
// Concept B behaviour deliberately NOT carried into ui.cpp (Dough, 2026-09-23).
// Two things differ from ui.cpp besides the look: touch comes from CtpTouch, and begin()
// turns display inversion off.
//
// Screen rendering and touch input — implements Section 14 (UI Design).
//
// Owns the TFT_eSPI instance and the touch calibration. Reads Station for what to
// draw and never stores application state of its own beyond what is needed to know
// whether the screen is already showing the right thing.
//
// STEPS A-E of the UI build: main screen, partial redraw, sleep/wake, the start-slot
// flow, the cancel confirmation, the finished-cure alarm, the menu and its keyboards,
// and the power-loss warning that step E's persistence needs.
//
// Pins come from TFT_eSPI's User_Setup.h, never from here (hard rule 4).
#pragma once

#include <Arduino.h>

namespace UIFuji {

// Blank the panel after this long with no touch, to avoid a static image sitting on
// the LCD all day. 3 minutes, as requested. Since 2026-09-23 only while NO CURE IS RUNNING
// (the mat may be on) — the count starts when the last cure ends —
// and by filling black, without DISPOFF (this module shows white on DISPOFF). Mat ON: a dim
// FUJIFILM wordmark that moves to a random spot every minute. Mat OFF: plain black.
//
// FACT: this blanks the PIXELS only. The backlight LED pin is hardwired to 3V3 on
// this build (TFT_BL deliberately undefined in User_Setup.h), so it cannot be
// switched off in software — the panel goes black but stays lit.
//
// TRIED AND REVERTED 2026-08-21: LED wire moved to GPIO22 and driven from begin()/
// goToSleep()/wake(). It did not work on hardware. Read the backlight section of
// Project_Status.md before attempting this again — the manual says the LED pin only
// GATES a VCC-powered backlight, so the theory was sound and the failure is not yet
// explained. Do not retry it blind.
constexpr uint32_t SLEEP_AFTER_MS = 3UL * 60UL * 1000UL;

void begin();

// Overrides the sleep timeout. Exists so bring-up testing does not have to sit
// through three minutes per attempt — production code should leave the default alone.
void setSleepTimeout(uint32_t ms);

// Call every loop. Handles touch, the sleep timer, and redrawing whatever changed.
void service();

// Feed one key from the physical 4x3 keypad ('0'-'9', '*', '#'); 0 means no key, so
// KeypadDriver::poll() can be passed straight in. Follows key() in the Concept B HTML
// (handoff 10): on Home '#' starts a cure, 1-6 act on that slot, '0' clears the finished
// ones and '*' opens Settings; on a subpage digits type into number screens or pick the
// numbered row, 7/9 page lists, '#' confirms (Skip on Curing position) and '*' goes back
// one level.
//
// The Home footer prints them — "Settings (*)", "Clear finished (0)", "+ New cure (#)" —
// which the Concept B HTML originally did not (added at Dough's request, 2026-09-23).
//
// Touch only: tapping the temperature/heating area in the header opens the heat off
// timer's minutes entry directly while the mat is ON, and the Heating mat page ("Turn heat
// ON", which then returns Home) while it is OFF. Back returns Home either way. The keypad
// route is Settings 5.
//
// Three deliberate differences from the mockup, all because the real part is 4x3:
//
//  1. There is no A/B/C/D column, so the mockup's 2/8 list navigation with a moving
//     focus is not implemented — a digit selects its list row directly instead. That
//     needs no focus indicator to be drawn, and the rows are numbered on screen
//     anyway. Numbering on every list is PER PAGE, not absolute, and where a list
//     pages, '7' and '9' are prev/next (printed on the buttons) — without them the
//     rows past page 1 could not be reached from the keypad at all.
//  2. On the cancel confirmation, the heat warning and the power-loss page, '#' does
//     nothing (the HTML agrees). They put the safe choice on 1 precisely so a stray
//     confirm cannot destroy work; a blanket '#'=OK would defeat that. Press 1 or 2.
//  3. The name keyboard is touch-only — a numeric pad cannot type letters. Pointer
//     navigation with 2/4/6/8 was considered and rejected 2026-08-06: the keyboard's
//     own top row IS "1234567890", so those digits cannot mean both "move" and "type",
//     and a name is ~30 pointer presses against ~7 taps. Adding a user is the rarest
//     action in the UI. Consequence accepted knowingly: the station CANNOT be operated
//     entirely without touch — cures can be run, cancelled and acknowledged from the
//     keypad, but a new user cannot be added.
//
// On the ManageUsers list a digit opens a per-user popup (change tone / delete) rather
// than acting directly, because touch has two targets on that row and a digit cannot
// say which was meant. Touch still goes straight to either. '#' adds a user.
//
// On the alarm popup '#' stops the top card (the oldest unacknowledged cure), and a
// digit stops the card for that SLOT number — not the card's position, because
// position shifts as other cures are acknowledged while the slot number never does.
// Each card shows its slot digit ("Stop (3)", as in Concept B) — or "Stop (#)" when it
// is the only card; '#' always takes the oldest card. One press acknowledges exactly one cure — there is
// deliberately no key that silences everything at once, and no '*' back. The × on each
// card acknowledges that card only, same as tapping the card.
void handleKey(char k);

// Header values, supplied by the caller so this module stays independent of the
// sensor. Cheap to call every loop — it only redraws when the value actually moves.
void setHeader(float currentTempC, bool heatOn, bool tempValid);

// Shows the "power was interrupted" warning over the restored cures. Call once at
// boot, after begin(), when Persist::begin() reported a cure came back running.
// Passed in by the caller rather than read from Persist directly, for the same
// reason as setHeader(): this module stays independent of where the data came from.
void showPowerLossWarning();

// True while that warning waits for 1 (keep) or 2 (clear). The sketch skips
// Station::tickOneSecond() meanwhile, so the cure countdowns and the heat off timer hold
// until someone answers; the heater keeps holding the setpoint (Dough, 2026-09-23).
bool holdingCountdowns();

// Acknowledges the oldest unacknowledged alarm, same as pressing '#' on the alarm
// popup — for a trigger that isn't a keypress or a touch, e.g. an ESP-NOW stop
// button. A no-op if nothing is currently sounding. Safe to call from any screen:
// the popup only ever opens from Main (see handleKey's comment on Screen::Alarm),
// so this acknowledges Station's queue regardless, and only touches the display if
// the popup happens to be the screen showing.
void remoteStopAlarm();

// The sound index whose play button was tapped on a tone list since the last call, or -1.
// Consumed on read. UIFuji has no hardware, so the sketch does the playing, the same split
// as the alarm sound (serviceAlarmSound in Test_ui_fuji).
int8_t takeTonePreview();

// Over-temperature warning (Dough, 2026-10-01). The sketch decides WHEN (air above OVERTEMP_C for
// OVERTEMP_HOLD_MS without a break) and calls setOverTemp() every second; this module only shows it.
// While active the red full-screen warning replaces whatever was showing (wakes a blank panel too)
// and cannot be dismissed: any key, any tap or the ESP-NOW button only SILENCES the sound
// (overTempSilenced() turns true). The warning closes by itself once the air is back below
// OVERTEMP_C, returning Home — or to the power-loss question if that was open. A cure that finishes
// meanwhile waits and opens its own popup afterwards. Normal overshoot peaks at ~52.8 C (logged
// runs, 2026-09-30/10-01), so this only fires when control has failed, e.g. an SSR stuck ON.
// It cannot cut the power itself: the mains plug, the fuse and the mat's own thermostat do that.
constexpr float OVERTEMP_C = 55.0f;
constexpr uint32_t OVERTEMP_HOLD_MS = 5UL * 60UL * 1000UL;
void setOverTemp(bool active, float tempC);
bool overTempSilenced();

// Sensor-fault warning (Dough, 2026-10-06, after T9). The sketch decides WHEN (no valid reading
// for SENSORFAULT_HOLD_MS without a break) and calls setSensorFault() every second. Same rules as
// the over-temperature warning: red full screen, wakes the panel, any key / tap / ESP-NOW press
// only silences, it closes by itself when the reading comes back. Why: with no reading the
// heater is already off, but the cure timers keep counting and would end in a normal "Cure
// complete" for parts that were never heated. The over-temperature warning outranks this one —
// the sketch does not raise both at once.
constexpr uint32_t SENSORFAULT_HOLD_MS = 10UL * 1000UL;
void setSensorFault(bool active);
bool sensorFaultSilenced();

// True while the panel is blanked. Nothing is drawn in this state.
bool asleep();

// Wake and redraw. Called on touch; also call this when an alarm fires, so a finished
// cure is never sitting behind a blank screen.
void wake();

// Forces the next service() to repaint everything rather than just what changed.
void requestFullRedraw();

} // namespace UIFuji
