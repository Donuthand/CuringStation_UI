// Non-volatile storage — the user list, the two settings and the six slots kept
// across a power cut, using NVS through the ESP32 Preferences library. Step E of
// the UI build; implements the "restore and warn" decision in Project_Status.md.
//
// Contains NO display code and no timer logic: it reads and writes Station and
// nothing else. Station itself knows nothing about flash, which is what keeps the
// timer/alarm logic testable on its own.
//
// FACT: this board has no battery-backed clock. Slots come back with the remaining
// time they had at the last save, so after an outage every countdown is SHORT by
// however long the power was off, and there is no way to correct that here. That
// is why begin() reports it and the UI warns rather than silently resuming.
#pragma once

#include <Arduino.h>

namespace Persist {

// The countdowns are rewritten no more often than this. See persist.cpp for the
// flash-wear arithmetic behind the number.
constexpr uint32_t SAVE_INTERVAL_MS = 30000;

struct Restored {
  bool config;   // a saved user list + settings was found and loaded
  bool running;  // at least one cure came back still counting down
};

// Opens NVS and loads whatever is stored into Station. Call AFTER Station::begin()
// — begin() wipes everything, so restoring before it would be thrown away.
Restored begin();

// Call every loop. Writes only when something actually changed; starting,
// cancelling or finishing a cure is written straight away, a ticking countdown at
// most once per SAVE_INTERVAL_MS.
void service();

} // namespace Persist
