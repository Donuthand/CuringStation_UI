// See matrix_keypad.h for which spec sections/gaps this implements.
#include "matrix_keypad.h"
#include <Keypad.h>

static const byte ROWS = 4;
static const byte COLS = 3;

static char keys[ROWS][COLS] = {
  {'1', '2', '3'},
  {'4', '5', '6'},
  {'7', '8', '9'},
  {'*', '0', '#'}
};

// VERIFIED ON HARDWARE 2026-09-25 (all 12 keys). REWIRED AND RE-DERIVED that day (Dough changed
// the keypad wiring; Test_keypad_discover,
// keys pressed 1-9 then * 0 #). Same seven GPIOs, new roles. Dead maps, do not restore:
// 2026-09-07 fish board (rows 33/21/14/26, cols 25/32/27) and 2026-08-24 breadboard
// (rows 27/21/32/25, cols 26/14/33).
//
//   Row 1 = GPIO 14      Col 1 = GPIO 27
//   Row 2 = GPIO 32      Col 2 = GPIO 21
//   Row 3 = GPIO 33      Col 3 = GPIO 25
//   Row 4 = GPIO 26
//
// Self-check on the 2026-09-25 data: the twelve presses block 3-3-3-3 by row pin
// (14, 32, 33, 26) and the column pins repeat 27, 21, 25 inside every block.
//
// Method, worth keeping because three earlier attempts got this wrong. Test_keypad_discover
// reports only which two GPIOs a press shorts — it cannot tell a row from a column, and it
// cannot give the ORDER. The row/column split IS unambiguous from the data: on a 4x3 pad
// every press joins one of four pins to one of three, so the group of four are the rows.
// The order comes from pressing keys whose position is known, 1-9 then * 0 #: the twelve
// presses block 3-3-3-3 by row pin, and within every block the column pins repeat in the
// same order, which is the self-check. Row 1 was read off keys 1/2/3 and the resulting map
// then PREDICTED all nine remaining keys.
//
// Note the pad numbers Test_keypad_discover prints are STALE — its padPins[] array still
// encodes the pre-2026-08-23 wiring, so "pad 4" is just an alias for GPIO 26 and says
// nothing about which physical pad that is. Read the GPIO numbers, ignore the pad labels.
//
// All 12 keys responded: no dead pad on this board.
static byte rowPins[ROWS] = {14, 32, 33, 26};
static byte colPins[COLS] = {27, 21, 25};

static Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

namespace KeypadDriver {

void begin() {
  // The Keypad library defaults to 10 ms of debounce, which suits a snap-action tactile
  // switch. The KEY-34 is not one: its key sites are open interdigitated comb pads closed
  // by a carbon pill on the silicone keymat, so a press that lands off-centre or rolls
  // makes and breaks several times on the way down and registers as a double. Reported
  // 2026-09-07, with all 12 keys already reading CORRECTLY - which is what rules the
  // matrix wiring out, since a bad joint drops or misreads keys rather than duplicating
  // the right one.
  //
  // 25 ms is chosen to sit well clear of a deliberate fast double-press: repeated human
  // presses land 150-250 ms apart, so this cannot eat one. Raise toward 40 ms if doubles
  // survive; do NOT go past ~80 ms, where entering "11" starts to lose a digit.
  keypad.setDebounceTime(25);
}

char poll() {
  return keypad.getKey(); // returns 0 (NO_KEY) if nothing pressed
}

} // namespace KeypadDriver
