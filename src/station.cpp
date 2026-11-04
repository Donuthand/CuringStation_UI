// See station.h for which spec sections this implements.
#include "station.h"

namespace Station {

// Plain "Tone N" names for all ten (Dough, 2026-09-25) — they were Chime, Bell, Beep, Buzz,
// Horn + five placeholders. Saved users keep their tone: Persist stores the index, not the name.
const char* const SOUND_NAMES[SOUND_COUNT] = { "Tone 1", "Tone 2", "Tone 3", "Tone 4", "Tone 5",
                                               "Tone 6", "Tone 7", "Tone 8", "Tone 9", "Tone 10" };
const char* const POSITION_NAMES[POSITION_COUNT] = { "Lens", "RBS", "Tube", "Cap", "Handle cover" };

} // namespace Station

static Station::Slot slots[Station::SLOT_COUNT];

static Station::User users[Station::MAX_USERS];
static uint8_t userN = 0;

static uint8_t setpoint = 45;      // mockup default, inside the 40-50 lock
static uint8_t alarmSec = 15;      // mockup default
static uint8_t alarmVol = Station::ALARM_VOL_DEFAULT;

static bool heatOn = true;         // see Station::heatEnabled()
static uint32_t heatOffSec = 0;    // off timer, 0 = none

// See Station::configRevision(). Only bumped when a value actually moves, so
// re-confirming the same setpoint does not cost a flash write.
static uint32_t configRev = 0;

// Slot indices that finished and have not been acknowledged, oldest first. A queue
// rather than a flag because two slots can finish while nobody is at the machine,
// and each one still has to be shown and acknowledged separately.
static uint8_t alarmQueue[Station::SLOT_COUNT];
static uint8_t alarmN = 0;

// Rolling temperature history, oldest first. See station.h for why it stays in RAM.
static float tempLog[Station::TEMP_LOG_MAX];
static uint16_t tempLogN = 0;
static uint16_t tempLogTick = 0;
static uint16_t tempLogInterval = Station::TEMP_LOG_INTERVAL_SEC;

static void alarmEnqueue(uint8_t i) {
  for (uint8_t k = 0; k < alarmN; k++) {
    if (alarmQueue[k] == i) return; // already waiting
  }
  if (alarmN < Station::SLOT_COUNT) alarmQueue[alarmN++] = i;
}

static void alarmRemove(uint8_t i) {
  for (uint8_t k = 0; k < alarmN; k++) {
    if (alarmQueue[k] != i) continue;
    for (uint8_t j = k; j + 1 < alarmN; j++) alarmQueue[j] = alarmQueue[j + 1];
    alarmN--;
    return;
  }
}

static void copyName(char* dst, const char* src) {
  strncpy(dst, src, Station::USER_NAME_MAX);
  dst[Station::USER_NAME_MAX] = '\0';
}

namespace Station {

void begin() {
  for (uint8_t i = 0; i < SLOT_COUNT; i++) slots[i] = Slot();
  userN = 0;
  alarmN = 0;
  tempLogN = 0;
  tempLogTick = 0;
  heatOn = true;
  heatOffSec = 0;
  configRev++;   // begin() wipes the user list, which is a config change like any other
}

const Slot& slot(uint8_t i) {
  return slots[i < SLOT_COUNT ? i : 0];
}

bool startSlot(uint8_t i, const char* name, uint8_t sound, uint16_t minutes,
               uint8_t position) {
  if (i >= SLOT_COUNT) return false;
  if (name == nullptr || name[0] == '\0') return false;
  if (minutes < TIME_MIN_MIN || minutes > TIME_MAX_MIN) return false;

  alarmRemove(i); // restarting a done slot must not leave its old alarm queued
  slots[i].state = SlotState::Running;
  copyName(slots[i].name, name);
  slots[i].sound = sound < SOUND_COUNT ? sound : 0;
  slots[i].totalSec = (uint32_t)minutes * 60UL;
  slots[i].remainingSec = slots[i].totalSec;
  slots[i].position = position <= POSITION_COUNT ? position : 0;
  return true;
}

void cancelSlot(uint8_t i) {
  if (i >= SLOT_COUNT) return;
  alarmRemove(i);
  slots[i] = Slot();
}

void clearDone(uint8_t i) {
  if (i >= SLOT_COUNT) return;
  if (slots[i].state != SlotState::Done) return;
  alarmRemove(i);
  slots[i] = Slot();
}

void clearAllDone() {
  for (uint8_t i = 0; i < SLOT_COUNT; i++) clearDone(i);
}

void tickOneSecond() {
  for (uint8_t i = 0; i < SLOT_COUNT; i++) {
    if (slots[i].state != SlotState::Running) continue;
    if (slots[i].remainingSec > 0) slots[i].remainingSec--;
    if (slots[i].remainingSec == 0) {
      slots[i].state = SlotState::Done;
      alarmEnqueue(i);
    }
  }

  // The off timer running out switches the mat off for good — only setHeatEnabled()
  // turns it back on. Counted as a config change so Persist writes the flag at once.
  if (heatOffSec > 0 && --heatOffSec == 0 && heatOn) {
    heatOn = false;
    configRev++;
  }
}

void restoreSlot(uint8_t i, SlotState st, const char* name, uint8_t sound,
                 uint32_t totalSec, uint32_t remainingSec, uint8_t position) {
  if (i >= SLOT_COUNT) return;

  slots[i] = Slot();
  alarmRemove(i);
  if (st == SlotState::Empty || name == nullptr || name[0] == '\0') return;

  copyName(slots[i].name, name);
  slots[i].sound = sound < SOUND_COUNT ? sound : 0;
  slots[i].totalSec = totalSec;
  slots[i].remainingSec = remainingSec > totalSec ? totalSec : remainingSec;
  slots[i].position = position <= POSITION_COUNT ? position : 0;

  // A Running slot with nothing left is a cure that hit zero between the last save
  // and the power going out. It is Done, not stuck at 00:00 forever.
  if (st == SlotState::Done || slots[i].remainingSec == 0) {
    slots[i].state = SlotState::Done;
    slots[i].remainingSec = 0;
    alarmEnqueue(i);
  } else {
    slots[i].state = SlotState::Running;
  }
}

bool hasPendingAlarm()      { return alarmN > 0; }
int8_t pendingAlarmSlot()   { return alarmN > 0 ? (int8_t)alarmQueue[0] : -1; }
int8_t pendingAlarmAt(uint8_t k) { return k < alarmN ? (int8_t)alarmQueue[k] : -1; }
uint8_t pendingAlarmCount() { return alarmN; }
void acknowledgeAlarm(uint8_t i) { alarmRemove(i); }

uint8_t userCount() { return userN; }

const User& user(uint8_t i) {
  return users[i < userN ? i : 0];
}

bool addUser(const char* name, uint8_t sound) {
  if (userN >= MAX_USERS) return false;
  if (name == nullptr || name[0] == '\0') return false;
  copyName(users[userN].name, name);
  users[userN].sound = sound < SOUND_COUNT ? sound : 0;
  userN++;
  configRev++;
  return true;
}

void deleteUser(uint8_t i) {
  if (i >= userN) return;
  for (uint8_t k = i; k + 1 < userN; k++) users[k] = users[k + 1];
  userN--;
  configRev++;
}

void setUserSound(uint8_t i, uint8_t sound) {
  if (i >= userN || sound >= SOUND_COUNT) return;
  if (users[i].sound == sound) return;
  users[i].sound = sound;
  configRev++;
}

void sampleTemperature(float c) {
  if (++tempLogTick < tempLogInterval) return;
  tempLogTick = 0;
  if (tempLogN < TEMP_LOG_MAX) {
    tempLog[tempLogN++] = c;
    return;
  }
  for (uint16_t k = 0; k + 1 < TEMP_LOG_MAX; k++) tempLog[k] = tempLog[k + 1];
  tempLog[TEMP_LOG_MAX - 1] = c;
}

uint16_t tempLogCount() { return tempLogN; }
float tempLogAt(uint16_t k) { return k < tempLogN ? tempLog[k] : 0.0f; }
uint16_t tempLogIntervalSec() { return tempLogInterval; }
void setTempLogIntervalSec(uint16_t s) { if (s > 0) tempLogInterval = s; }

uint8_t setpointC() { return setpoint; }

void setSetpointC(uint8_t c) {
  if (c < SETPOINT_MIN_C) c = SETPOINT_MIN_C;
  if (c > SETPOINT_MAX_C) c = SETPOINT_MAX_C;
  if (setpoint == c) return;
  setpoint = c;
  configRev++;
}

uint8_t alarmDurationSec() { return alarmSec; }

void setAlarmDurationSec(uint8_t s) {
  if (s < ALARM_MIN_SEC) s = ALARM_MIN_SEC;
  if (s > ALARM_MAX_SEC) s = ALARM_MAX_SEC;
  if (alarmSec == s) return;
  alarmSec = s;
  configRev++;
}

uint8_t alarmVolume() { return alarmVol; }

void setAlarmVolume(uint8_t v) {
  if (v < ALARM_VOL_MIN) v = ALARM_VOL_MIN;
  if (v > ALARM_VOL_MAX) v = ALARM_VOL_MAX;
  if (alarmVol == v) return;
  alarmVol = v;
  configRev++;
}

uint32_t configRevision() { return configRev; }

bool heatEnabled() { return heatOn; }

void setHeatEnabled(bool on) {
  heatOffSec = 0;
  if (heatOn == on) return;
  heatOn = on;
  configRev++;
}

bool startHeatOffTimer(uint16_t minutes) {
  if (minutes < TIME_MIN_MIN || minutes > TIME_MAX_MIN) return false;
  // Concept B (2026-09-23): setting a timer is an explicit request for heat until then, so
  // a disabled mat comes back ON with it. Counted as a config change so Persist writes
  // the flag at once.
  if (!heatOn) { heatOn = true; configRev++; }
  heatOffSec = (uint32_t)minutes * 60UL;
  return true;
}

void cancelHeatOffTimer() { heatOffSec = 0; }

uint32_t heatOffRemainingSec() { return heatOffSec; }

void restoreHeatOffTimer(uint32_t sec) {
  if (!heatOn) return;
  uint32_t maxSec = (uint32_t)TIME_MAX_MIN * 60UL;
  heatOffSec = sec > maxSec ? maxSec : sec;
}

int8_t longestRunningSlot() {
  int8_t best = -1;
  for (uint8_t i = 0; i < SLOT_COUNT; i++) {
    if (slots[i].state != SlotState::Running) continue;
    if (best < 0 || slots[i].remainingSec > slots[best].remainingSec) best = (int8_t)i;
  }
  return best;
}

// First letter of each word, up to 2. "User B" -> "UB", "Dough" -> "D".
void initialsOf(const char* name, char* out) {
  uint8_t n = 0;
  bool atWordStart = true;
  for (const char* p = name; *p && n < 2; p++) {
    bool isLetter = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z');
    if (!isLetter) { atWordStart = true; continue; }
    if (atWordStart) {
      out[n++] = (*p >= 'a') ? (*p - 32) : *p;
      atWordStart = false;
    }
  }
  if (n == 0) { out[n++] = '?'; }
  out[n] = '\0';
}

void formatRemaining(uint32_t sec, char* out) {
  uint32_t h = sec / 3600;
  uint32_t m = (sec % 3600) / 60;
  uint32_t s = sec % 60;
  if (h > 0) snprintf(out, 9, "%lu:%02lu:%02lu", h, m, s);
  else       snprintf(out, 9, "%02lu:%02lu", m, s);
}

} // namespace Station
