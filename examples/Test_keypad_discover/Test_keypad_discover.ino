// Pinout discovery for the salvaged KEY-34 PCB keypad (Build Order step 3).
//
// The KEY-34 has no datasheet and its 7 signal pads are in an unknown order, so
// the row/col arrays in matrix_keypad.cpp cannot be written until the matrix is
// known. This sketch finds it: it drives each pad LOW in turn, reads the rest,
// and prints whichever pair is shorted. Press a key (or bridge a key's two comb
// pads with a wire) and the pair for that key is printed once.
//
// 7 signal lines for 12 keys means a plain 4x3 matrix, so the pins below are the
// ones matrix_keypad.cpp already uses - only their ROLES (which is a row, which
// is a column) are unknown, and that is what this finds.
//
// How to read the output: on a 4x3 matrix every press joins one of four pins to one
// of three, so after 12 presses the group of FOUR is the rows - that part needs no
// assumption. The ORDER does: press 1-9 then * 0 #, and the pairs should block
// 3-3-3-3 by row pin with the column pins repeating in the same order inside every
// block. If they do not, the press order is what to doubt first.
//
// Standalone on purpose - no Firmware/src modules, so no --library flag needed.
// Serial at 115200.

const int PAD_COUNT = 7;

// The seven GPIOs the keypad's signal pads land on. This is a SET, not an order:
// the array index is just probe order and carries no meaning, so this sketch prints
// GPIO numbers only. It used to print "pad N" as well, claiming index N-1 was the
// Nth pad left-to-right along the board's bottom edge. That was true of the original
// breadboard wiring and survived two rewires as a lie - by 2026-09-07 "pad 4" meant
// nothing but "GPIO 26". Nothing here can learn physical pad positions, so it no
// longer pretends to. If you need them, meter the edge pads to the header.
//
// The same seven pins survived the 2026-09-07 fish-board rewire; only their roles
// moved. Deliberately NOT printed at runtime - this sketch exists to find the matrix
// from scratch, and labelling its output with the map you already believe is how you
// confirm a wrong map. Read the raw pairs, then compare against matrix_keypad.cpp.
const int padPins[PAD_COUNT] = {32, 33, 25, 26, 27, 14, 21};

int lastA = -1, lastB = -1;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("KEY-34 pinout discovery. Press one key at a time.");
  for (int i = 0; i < PAD_COUNT; i++) pinMode(padPins[i], INPUT_PULLUP);
}

void loop() {
  int foundA = -1, foundB = -1;

  for (int i = 0; i < PAD_COUNT && foundA < 0; i++) {
    pinMode(padPins[i], OUTPUT);
    digitalWrite(padPins[i], LOW);
    delayMicroseconds(50); // let the line settle before reading

    for (int j = 0; j < PAD_COUNT; j++) {
      if (j == i) continue;
      if (digitalRead(padPins[j]) == LOW) { foundA = i; foundB = j; break; }
    }
    pinMode(padPins[i], INPUT_PULLUP);
  }

  // Print only on change, so holding a key does not flood the monitor.
  if (foundA != lastA || foundB != lastB) {
    if (foundA >= 0) {
      Serial.print("closed: GPIO ");
      Serial.print(padPins[foundA]);
      Serial.print(" <-> GPIO ");
      Serial.println(padPins[foundB]);
    } else {
      Serial.println("-- released --");
    }
    lastA = foundA;
    lastB = foundB;
  }

  delay(30);
}
