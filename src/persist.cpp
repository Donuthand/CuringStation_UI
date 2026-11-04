// See persist.h for what this is and why Station knows nothing about it.
#include "persist.h"
#include "station.h"

#include <Preferences.h>

namespace {

// NVS namespace and key names — both are limited to 15 characters.
constexpr const char* NS       = "epoxy";
constexpr const char* KEY_CFG  = "cfg";
constexpr const char* KEY_SLOT = "slots";
// The heating mat's on/off flag and off timer. Separate keys rather than new fields in
// the blobs: a blob layout change needs FORMAT_VERSION bumped, and that would throw
// away the saved users and cures on the first boot of this firmware.
constexpr const char* KEY_HEAT     = "heat";     // 1 = enabled, 0 = switched off
constexpr const char* KEY_HEAT_SEC = "heatsec";  // off timer seconds left, 0 = none
constexpr const char* KEY_VOL      = "vol";      // alarm volume, same reasoning (2026-09-24)

// Bump this when either blob layout below changes. A version or length mismatch
// makes the stored blob be ignored, rather than read back as garbage into a slot.
//
// NOT bumped for the curing position (2026-09-23): the one number covers both blobs, so a
// bump would also throw away the users and settings. Adding the field changed the slot
// blob's LENGTH, which the length check already rejects - saved cures are dropped once on
// the first boot of that firmware, users and settings survive (decided with Dough).
constexpr uint8_t FORMAT_VERSION = 1;

// These two go into flash byte for byte, so the layout is part of the stored
// format — packed, and deliberately NOT the Station structs, so that reordering a
// field in station.h cannot silently change what is on disk.
struct __attribute__((packed)) CfgBlob {
  uint8_t version;
  uint8_t setpointC;
  uint8_t alarmSec;
  uint8_t userCount;
  struct __attribute__((packed)) UserEntry {
    uint8_t sound;
    char    name[Station::USER_NAME_MAX + 1];
  } users[Station::MAX_USERS];
};

struct __attribute__((packed)) SlotBlob {
  uint8_t version;
  struct __attribute__((packed)) Entry {
    uint8_t  state;
    uint8_t  sound;
    uint32_t totalSec;
    uint32_t remainingSec;
    char     name[Station::USER_NAME_MAX + 1];
    uint8_t  position;   // 0 = none, added 2026-09-23 (Concept B)
  } slots[Station::SLOT_COUNT];
};

Preferences prefs;
bool opened = false;

uint32_t lastCfgRev = 0;
uint32_t lastSlotSaveMs = 0;

// What was last written, so an unchanged second does not cost a flash write.
Station::Slot slotShadow[Station::SLOT_COUNT];
uint32_t heatSecShadow = 0;

void snapshotSlots() {
  for (uint8_t i = 0; i < Station::SLOT_COUNT; i++) slotShadow[i] = Station::slot(i);
  heatSecShadow = Station::heatOffRemainingSec();
}

// Anything other than the countdown moving: a cure started, cancelled, cleared or
// finished. Rare, and worth writing the instant it happens.
bool structurallyDiffers() {
  // The heat off timer is the same idea: set, changed (only a new timer can make it
  // go UP), cancelled or run out are written at once; ticking down waits for the 30 s.
  uint32_t h = Station::heatOffRemainingSec();
  if (h > heatSecShadow || ((h == 0) != (heatSecShadow == 0))) return true;

  for (uint8_t i = 0; i < Station::SLOT_COUNT; i++) {
    const Station::Slot& s = Station::slot(i);
    if (slotShadow[i].state != s.state) return true;
    if (slotShadow[i].totalSec != s.totalSec) return true;
    if (slotShadow[i].sound != s.sound) return true;
    if (strcmp(slotShadow[i].name, s.name) != 0) return true;
    if (slotShadow[i].position != s.position) return true;
  }
  return false;
}

bool countdownDiffers() {
  if (heatSecShadow != Station::heatOffRemainingSec()) return true;
  for (uint8_t i = 0; i < Station::SLOT_COUNT; i++) {
    if (slotShadow[i].remainingSec != Station::slot(i).remainingSec) return true;
  }
  return false;
}

void saveConfig() {
  CfgBlob b = {};
  b.version   = FORMAT_VERSION;
  b.setpointC = Station::setpointC();
  b.alarmSec  = Station::alarmDurationSec();
  b.userCount = Station::userCount();
  for (uint8_t i = 0; i < b.userCount; i++) {
    const Station::User& u = Station::user(i);
    b.users[i].sound = u.sound;
    strncpy(b.users[i].name, u.name, Station::USER_NAME_MAX);
    b.users[i].name[Station::USER_NAME_MAX] = '\0';
  }
  prefs.putBytes(KEY_CFG, &b, sizeof(b));
  prefs.putUChar(KEY_HEAT, Station::heatEnabled() ? 1 : 0);
  prefs.putUChar(KEY_VOL, Station::alarmVolume());
}

void saveSlots() {
  SlotBlob b = {};
  b.version = FORMAT_VERSION;
  for (uint8_t i = 0; i < Station::SLOT_COUNT; i++) {
    const Station::Slot& s = Station::slot(i);
    b.slots[i].state        = (uint8_t)s.state;
    b.slots[i].sound        = s.sound;
    b.slots[i].totalSec     = s.totalSec;
    b.slots[i].remainingSec = s.remainingSec;
    strncpy(b.slots[i].name, s.name, Station::USER_NAME_MAX);
    b.slots[i].name[Station::USER_NAME_MAX] = '\0';
    b.slots[i].position     = s.position;
  }
  prefs.putBytes(KEY_SLOT, &b, sizeof(b));
  prefs.putULong(KEY_HEAT_SEC, Station::heatOffRemainingSec());
  snapshotSlots();
  lastSlotSaveMs = millis();
}

} // namespace

namespace Persist {

Restored begin() {
  Restored r = { false, false };

  if (!prefs.begin(NS, false)) {
    Serial.println("[persist] NVS open failed - running without saved state");
    return r;
  }
  opened = true;

  CfgBlob c = {};
  if (prefs.getBytesLength(KEY_CFG) == sizeof(c) &&
      prefs.getBytes(KEY_CFG, &c, sizeof(c)) == sizeof(c) &&
      c.version == FORMAT_VERSION) {
    Station::setSetpointC(c.setpointC);          // clamped by Station, not trusted here
    Station::setAlarmDurationSec(c.alarmSec);
    uint8_t n = c.userCount <= Station::MAX_USERS ? c.userCount : Station::MAX_USERS;
    for (uint8_t i = 0; i < n; i++) {
      c.users[i].name[Station::USER_NAME_MAX] = '\0';  // flash is never trusted to
      Station::addUser(c.users[i].name, c.users[i].sound);  // have terminated it
    }
    r.config = true;
    Serial.printf("[persist] restored %u users, setpoint %u C, alarm %u s\n",
                  n, Station::setpointC(), Station::alarmDurationSec());
  }

  // Missing keys (a board that never ran this firmware) read back as "on, no timer",
  // which is exactly how the station behaved before the off timer existed.
  Station::setHeatEnabled(prefs.getUChar(KEY_HEAT, 1) != 0);
  Station::restoreHeatOffTimer(prefs.getULong(KEY_HEAT_SEC, 0));
  Station::setAlarmVolume(prefs.getUChar(KEY_VOL, Station::ALARM_VOL_DEFAULT));
  if (!Station::heatEnabled()) {
    Serial.println("[persist] heating mat restored switched OFF");
  } else if (Station::heatOffRemainingSec() > 0) {
    Serial.printf("[persist] heat off timer restored, %lu s left\n",
                  (unsigned long)Station::heatOffRemainingSec());
  }

  SlotBlob s = {};
  if (prefs.getBytesLength(KEY_SLOT) == sizeof(s) &&
      prefs.getBytes(KEY_SLOT, &s, sizeof(s)) == sizeof(s) &&
      s.version == FORMAT_VERSION) {
    for (uint8_t i = 0; i < Station::SLOT_COUNT; i++) {
      if (s.slots[i].state > (uint8_t)Station::SlotState::Done) continue;
      s.slots[i].name[Station::USER_NAME_MAX] = '\0';
      Station::restoreSlot(i, (Station::SlotState)s.slots[i].state, s.slots[i].name,
                           s.slots[i].sound, s.slots[i].totalSec,
                           s.slots[i].remainingSec, s.slots[i].position);
      if (Station::slot(i).state == Station::SlotState::Running) {
        r.running = true;
        Serial.printf("[persist] slot %u restored running, %lu s left\n",
                      i + 1, (unsigned long)Station::slot(i).remainingSec);
      }
    }
  }

  // Captured after restoring, so loading the saved values is not itself seen as a
  // change and immediately written straight back.
  lastCfgRev = Station::configRevision();
  snapshotSlots();
  lastSlotSaveMs = millis();
  return r;
}

// Flash wear, since a save every 30 s sounds alarming: the slot blob is ~170 bytes,
// NVS appends it as ~6 entries in a 4 kB page, so a page fills after roughly 20
// writes = 10 minutes, and the default 20 kB NVS partition rotates 5 pages. That is
// about one erase per page every 50 minutes of a cure actually running, against a
// 100k-cycle endurance — a working lifetime measured in years, not months. This is
// also why the temperature log is deliberately NOT persisted (station.h): it would
// add a write every 90 s forever, including overnight with nothing curing.
void service() {
  if (!opened) return;

  uint32_t rev = Station::configRevision();
  if (rev != lastCfgRev) {
    lastCfgRev = rev;
    saveConfig();
  }

  if (structurallyDiffers()) { saveSlots(); return; }

  if (millis() - lastSlotSaveMs >= SAVE_INTERVAL_MS && countdownDiffers()) saveSlots();
}

} // namespace Persist
