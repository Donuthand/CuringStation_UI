// 4x3 matrix keypad driver — implements the keypad half of Section 12 (Hybrid
// Input) and the numeric-entry parts of Section 13/14.
//
// ACTION NEEDED (carried over from Coding/Test_keyboard/Test_keyboard.ino, not yet
// applied to the spec doc): the purchased part is 4x3 (12 keys: 0-9, *, #), not the
// 4x4 part Section 9/12/13 assume. There is no physical A/B/C/D column, so this
// driver only exposes the keys that actually exist. Section 12's "Back/cancel = A"
// mapping and Section 13's A(back)/B(clear)/C(space) menu-key table have no
// hardware key to bind to yet — that's a menu-logic decision for whoever writes the
// menu module, not something this driver should invent. Flagging here instead of
// guessing a substitute.
#pragma once

#include <Arduino.h>

namespace KeypadDriver {

// Initializes the Keypad library with the wiring VERIFIED on hardware 2026-09-25 by a
// full 12-key labelled press test (after the rewire): Rows 14/32/33/26, Cols 27/21/25.
// This is the KEY-34's own arrangement — see matrix_keypad.cpp for how it was derived
// and for the older maps that must not be restored.
void begin();

// Non-blocking. Returns the pressed key ('0'-'9', '*', '#') or 0 if none.
char poll();

} // namespace KeypadDriver
