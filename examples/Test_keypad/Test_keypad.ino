// Keypad bring-up test — Build Order step 3 (Section 17), using the keypad.h/.cpp
// module instead of a bare Keypad-library sketch. Confirms all keys on the actual
// 4x3 part (see keypad.h ACTION NEEDED note re: 4x3 vs spec's 4x4).
#include <matrix_keypad.h>

void setup() {
  Serial.begin(9600);
  Serial.println("Keypad test ready. Press any key...");
  KeypadDriver::begin();
}

void loop() {
  char key = KeypadDriver::poll();
  if (key) {
    Serial.print("Key pressed: ");
    Serial.println(key);
  }
}
