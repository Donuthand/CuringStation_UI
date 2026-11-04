// ESP-NOW alarm-stop receiver — supersedes the BLE design in spec Section 11. Lets a
// battery ESP32-C3 button (Coding/Test_espnow_button) silence the alarm from across
// the room, without any pairing step or WiFi network: ESP-NOW is peer-to-peer over
// the radio the ESP32 already has, no router/AP involved.
//
// Deliberately dumb: on a valid STOP_ALARM packet from ANY sender, it acknowledges
// the oldest pending alarm via the sketch's stop callback — the same "any button clears
// any one alarm" rule Station's own queue already implements for the keypad and
// touch (Section 11's decision, carried over unchanged). No per-button pairing or
// slot mapping, on purpose: simpler firmware on both ends, and matches Dough's
// original call that button identity does not matter.
//
// Runs the station's WiFi radio in STA mode with no AP connection — ESP-NOW needs
// wifi initialised but does not join a network. This does not conflict with
// anything else in the firmware, which uses no WiFi.
#pragma once

#include <Arduino.h>

namespace RemoteStop {

// Starts WiFi in STA mode and ESP-NOW. Safe to call even if the radio is flaky —
// on failure this logs and leaves the alarm stoppable from the keypad/touch as
// before, the same "missing hardware costs only its own feature" pattern as
// Sound::begin(). Returns false if either step failed.
//
// onStop is what a valid packet does, e.g. UIFuji::remoteStopAlarm. It is passed in
// rather than hardwired because the dark UI (UI::) and Concept B (UIFuji::) each have
// their own; calling the wrong one acks in Station but leaves the other UI's alarm
// screen stale (found 2026-09-24).
bool begin(void (*onStop)());

// Call every loop. The receive callback runs on WiFi's own task, so it only sets a
// flag; poll() does the actual onStop() call on the main loop, the same
// split KeypadDriver/UI already use.
void poll();

} // namespace RemoteStop
