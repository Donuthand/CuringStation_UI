// Screen rendering and touch input — implements Section 14 (UI Design), following
// the layout and colour scheme of epoxy_curing_station_UI.html.
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

namespace UI {

// Blank the panel after this long with no touch, to avoid a static image sitting on
// the LCD all day. 3 minutes, as requested.
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
// KeypadDriver::poll() can be passed straight in. Follows physKey() in the mockup:
// on the main screen '#' starts a cure, 1-6 act on that slot and '0' clears the done
// ones; digits type into the numeric screens, '#' confirms and '*' goes back one level.
//
// The two main-screen shortcuts are printed on their buttons ("+ Start (#)", "Clear
// done (0)") — a key nobody can discover is worse than no key. Menu deliberately has
// none: '*' is the obvious candidate and is back everywhere else, so a reflex press on
// the main screen would fire the wrong thing.
//
// Three deliberate differences from the mockup, all because the real part is 4x3:
//
//  1. There is no A/B/C/D column, so the mockup's 2/8 list navigation with a moving
//     focus is not implemented — a digit selects its list row directly instead. That
//     needs no focus indicator to be drawn, and the rows are numbered on screen
//     anyway. Numbering on every list is PER PAGE, not absolute, and where a list
//     pages, '7' and '9' are prev/next (printed on the buttons) — without them the
//     rows past page 1 could not be reached from the keypad at all.
//  2. On the cancel confirmation and the power-loss warning, '#' does nothing. Both
//     screens put the safe choice on row 1 precisely so a stray confirm cannot
//     destroy work; a blanket '#'=OK would defeat that. Press 1 or 2.
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
// Each card is labelled with a key that works on it: the top one shows "Stop (#)",
// the rest show their slot digit, and the next card inherits the '#' as they clear.
// One press still acknowledges exactly one cure — there is deliberately no key that
// silences everything at once, and no '*' back, as the popup has no X.
void handleKey(char k);

// Header values, supplied by the caller so this module stays independent of the
// sensor. Cheap to call every loop — it only redraws when the value actually moves.
void setHeader(float currentTempC, bool heatOn, bool tempValid);

// Shows the "power was interrupted" warning over the restored cures. Call once at
// boot, after begin(), when Persist::begin() reported a cure came back running.
// Passed in by the caller rather than read from Persist directly, for the same
// reason as setHeader(): this module stays independent of where the data came from.
void showPowerLossWarning();

// Acknowledges the oldest unacknowledged alarm, same as pressing '#' on the alarm
// popup — for a trigger that isn't a keypress or a touch, e.g. an ESP-NOW stop
// button. A no-op if nothing is currently sounding. Safe to call from any screen:
// the popup only ever opens from Main (see handleKey's comment on Screen::Alarm),
// so this acknowledges Station's queue regardless, and only touches the display if
// the popup happens to be the screen showing.
void remoteStopAlarm();

// True while the panel is blanked. Nothing is drawn in this state.
bool asleep();

// Wake and redraw. Called on touch; also call this when an alarm fires, so a finished
// cure is never sitting behind a blank screen.
void wake();

// Forces the next service() to repaint everything rather than just what changed.
void requestFullRedraw();

} // namespace UI
