// Station data model — the 6 cure slots, the saved user list, the setpoint and the
// alarm queue. Implements the state behind Section 13 (Menu Structure) and Section 19
// (Software Features Summary).
//
// Deliberately contains NO display or TFT_eSPI code. The UI reads this and draws it;
// this file never draws. That keeps the timer/alarm logic testable on its own and
// stops screen work from creeping into the part that has to stay correct.
#pragma once

#include <Arduino.h>

namespace Station {

// 6 since 2026-10-09 (Dough + BME staff: the middle slot now holds the control probe; was 7
// from 2026-09-15). Changing this changes the size of Persist's slot blob, so saved cures from
// the old count are skipped once at boot (length check) — users and settings live in a
// separate blob and survive.
constexpr uint8_t SLOT_COUNT = 6;
constexpr uint8_t MAX_USERS  = 30;   // Section 19: "up to ~30 names"
constexpr uint8_t USER_NAME_MAX   = 14;   // matches the mockup's 14-char input limit
constexpr uint8_t SOUND_COUNT = 10;  // 5 -> 10 on 2026-09-25; Concept B shows them in 2 columns

// Cure time limits, from the mockup's validation (1-480 min = up to 8 h).
constexpr uint16_t TIME_MIN_MIN = 1;
constexpr uint16_t TIME_MAX_MIN = 480;

// Alarm duration limits, from the mockup's validation.
constexpr uint8_t ALARM_MIN_SEC = 5;
constexpr uint8_t ALARM_MAX_SEC = 60;

// Alarm volume, on the DFPlayer's own 0-30 scale (Dough, 2026-09-24). Floor of 10 so the
// alarm can never be set too quiet to hear; the default matches Sound::VOLUME_DEFAULT, so a
// station that never touches the setting sends no volume command beyond Sound::begin()'s.
// Capped at 20, not the module's 30 (Dough, 2026-09-25): less speaker current, so smaller dips
// on the shared 5 V and less heat in the DFPlayer's amp driving an out-of-spec 4 Ohm speaker.
constexpr uint8_t ALARM_VOL_MIN = 10;
constexpr uint8_t ALARM_VOL_MAX = 20;
constexpr uint8_t ALARM_VOL_DEFAULT = 20;

// Setpoint limits — Section 19, locked in code, not merely validated at the UI.
constexpr uint8_t SETPOINT_MIN_C = 40;   // 40 since 2026-10-12 (was 35): the service manual says 40-50 °C
constexpr uint8_t SETPOINT_MAX_C = 50;

// Index into this is what gets stored per user; the DFPlayer track number is the
// index + 1 once that module exists.
extern const char* const SOUND_NAMES[SOUND_COUNT];

// Curing position (Concept B, handoff 6): an optional label for which part of the scope is
// being cured. Descriptive only — it changes no duration, temperature or alarm. Stored as
// 0 = none, 1..POSITION_COUNT = POSITION_NAMES[position - 1].
constexpr uint8_t POSITION_COUNT = 5;
extern const char* const POSITION_NAMES[POSITION_COUNT];

enum class SlotState : uint8_t {
  Empty,    // nothing loaded
  Running,  // counting down
  Done      // hit zero, waiting to be cleared
};

struct Slot {
  SlotState state = SlotState::Empty;
  char name[USER_NAME_MAX + 1] = "";
  uint8_t sound = 0;
  uint32_t totalSec = 0;
  uint32_t remainingSec = 0;
  uint8_t position = 0;   // 0 = none, see POSITION_NAMES
};

struct User {
  char name[USER_NAME_MAX + 1] = "";
  uint8_t sound = 0;
};

void begin();

// --- slots ---
const Slot& slot(uint8_t i);
// `position` defaults to none, so ui.cpp (which has no position step) is unchanged.
bool startSlot(uint8_t i, const char* name, uint8_t sound, uint16_t minutes,
               uint8_t position = 0);
void cancelSlot(uint8_t i);   // stops a running cure, clears the slot
void clearDone(uint8_t i);    // clears one finished slot
void clearAllDone();          // the "Clear done" button

// Advances every Running slot by one second and queues any that hit zero.
// Call once per second, not once per loop.
void tickOneSecond();

// --- alarm queue: slots that finished and have not been acknowledged ---
bool hasPendingAlarm();
int8_t pendingAlarmSlot();      // oldest still waiting, -1 if none
int8_t pendingAlarmAt(uint8_t k); // k-th in queue order, oldest first; -1 past the end
uint8_t pendingAlarmCount();
void acknowledgeAlarm(uint8_t i);

// Restores one slot exactly as it was saved, part-elapsed countdown included.
// startSlot() cannot do this — it always resets remainingSec to the full time.
// A restored Done slot is re-queued for its alarm, so a cure that finished during
// an outage is still announced; the original queue order is not stored and falls
// back to slot order, which only shows if two finished unacknowledged.
void restoreSlot(uint8_t i, SlotState st, const char* name, uint8_t sound,
                 uint32_t totalSec, uint32_t remainingSec, uint8_t position = 0);

// --- users ---
uint8_t userCount();
const User& user(uint8_t i);
bool addUser(const char* name, uint8_t sound); // false if full or name empty
void deleteUser(uint8_t i);
void setUserSound(uint8_t i, uint8_t sound);

// --- temperature log ---
// A rolling buffer in RAM only. Deliberately NOT written to flash: a sample every
// 90 s would be ~40 writes an hour, and the ESP32's flash wears out. Nothing here
// survives a power cycle, which is acceptable — it is a "what has it been doing"
// display, not a record.
// uint16_t, not uint8_t — 320 does not fit in a byte and would wrap to 64.
constexpr uint16_t TEMP_LOG_MAX = 320;
constexpr uint16_t TEMP_LOG_INTERVAL_SEC = 90;  // 320 samples = 8 h, the longest cure

// Call once per second with the current reading. Appends one sample every
// TEMP_LOG_INTERVAL_SEC and drops the oldest once full. Pass NAN when the probe gave no
// valid reading: the sample is kept as a gap (Concept B draws the trace broken there
// rather than joined across it). Test_ui only ever passes valid readings, so ui.cpp's
// chart never sees a NAN.
void sampleTemperature(float c);
uint16_t tempLogCount();
float tempLogAt(uint16_t k);       // 0 = oldest; NAN for a gap
uint16_t tempLogIntervalSec();

// Bring-up only, so testing does not have to wait 90 s per point. Production code
// should leave the default alone. Same purpose as UI::setSleepTimeout().
void setTempLogIntervalSec(uint16_t s);

// --- settings ---
uint8_t setpointC();
void setSetpointC(uint8_t c);      // clamped to SETPOINT_MIN_C..SETPOINT_MAX_C
uint8_t alarmDurationSec();
void setAlarmDurationSec(uint8_t s); // clamped to ALARM_MIN_SEC..ALARM_MAX_SEC
uint8_t alarmVolume();
void setAlarmVolume(uint8_t v);      // clamped to ALARM_VOL_MIN..ALARM_VOL_MAX

// Bumped by every change to the user list or to the settings above. Persist
// polls this rather than each mutator having to call it, so a setting added later
// cannot silently end up not being saved.
uint32_t configRevision();

// --- heating mat on/off and its off timer ---
// Menu item 5. The mat is either enabled (the sketch lets Heater hold the setpoint) or
// switched off. The off timer counts down in tickOneSecond() and switches the mat off
// at zero, and it then STAYS off until someone turns it back on — it never re-enables
// by itself (Dough's choice, 2026-09-15). Nothing here touches the SSR: the sketch
// reads heatEnabled() and calls Heater::forceOff(), same split as the setpoint.
bool heatEnabled();
void setHeatEnabled(bool on);              // either way, clears any running off timer
// TIME_MIN_MIN..TIME_MAX_MIN, false if out of range. Turns a disabled mat back ON
// (Concept B: Home's heating area opens the timer directly, even with the mat off).
bool startHeatOffTimer(uint16_t minutes);
void cancelHeatOffTimer();
uint32_t heatOffRemainingSec();            // 0 = no timer running
// For Persist only: puts back a part-elapsed timer. Ignored while the mat is off.
void restoreHeatOffTimer(uint32_t sec);

// The running cure with the most time left, -1 if none. Used to warn when an off
// timer would cut the heat before that cure finishes.
int8_t longestRunningSlot();

// --- helpers the UI needs, kept here so the rules live in one place ---
// "User B" -> "UB". Writes 2 chars + NUL into out (min 3 bytes).
void initialsOf(const char* name, char* out);
// Formats remaining seconds as MM:SS, or H:MM:SS past an hour. out needs 9 bytes.
void formatRemaining(uint32_t sec, char* out);

} // namespace Station
