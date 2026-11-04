// Wire format for the station's ESP-NOW receiver (remote_stop.cpp). The button
// sketch (examples/Test_espnow_button) keeps its OWN copy of this exact file rather
// than including this one — it targets a different board (ESP32-C3) and pulling in
// this whole library for one header would drag in TFT_eSPI/keypad/sound modules it
// has no use for. If the two ever disagree, see that copy's header comment.
#pragma once

#include <stdint.h>

namespace RemoteStopProtocol {

// Filters our packets out of any other ESP-NOW traffic that happens to be on the
// same channel nearby. Bump VERSION, not MAGIC, if the payload shape ever changes —
// that way a button still on the old sketch is silently ignored instead of misread.
constexpr uint8_t MAGIC[3]      = {'C', 'U', 'R'};
constexpr uint8_t VERSION       = 1;
constexpr uint8_t CMD_STOP_ALARM = 1;

struct Packet {
  uint8_t magic[3];
  uint8_t version;
  uint8_t cmd;
};

} // namespace RemoteStopProtocol
