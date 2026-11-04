// COPY — must be kept byte-identical to Coding/Firmware/src/remote_stop_protocol.h.
//
// Not included from the library on purpose: this sketch targets an ESP32-C3, and
// pulling in the EpoxyCuringStation library for one header would drag in every
// other module (display/TFT_eSPI, keypad, sound...) that the station's ESP32-WROOM
// needs and this board does not — same duplication trade already made for
// User_Setup_epoxy.h vs TFT_eSPI's own User_Setup.h. If this file and the library's
// copy ever disagree, the station silently stops recognising this button's packets
// (version/magic mismatch — see remote_stop.cpp) rather than crashing, so this is
// worth checking by hand after any change to either copy.
#pragma once

#include <stdint.h>

namespace RemoteStopProtocol {

constexpr uint8_t MAGIC[3]      = {'C', 'U', 'R'};
constexpr uint8_t VERSION       = 1;
constexpr uint8_t CMD_STOP_ALARM = 1;

struct Packet {
  uint8_t magic[3];
  uint8_t version;
  uint8_t cmd;
};

} // namespace RemoteStopProtocol
