// See sound.h for the pins and the track mapping.
#include "sound.h"

#include <DFRobotDFPlayerMini.h>

namespace {

DFRobotDFPlayerMini dfp;

bool isOnline  = false;
bool isPlaying = false;
uint32_t stopAtMs = 0;
uint8_t lastVolume = Sound::VOLUME_DEFAULT;   // re-sent if the module restarts, see below
uint8_t lastTrack = 1;                        // re-started if the module restarts mid-tone
uint32_t restarts = 0;
uint32_t nextRetryMs = 0;                     // while offline: when poll() next resets the module
uint8_t retriesPrinted = 0;
constexpr uint32_t RETRY_MS = 5000;

} // namespace

namespace Sound {

bool begin() {
  Serial2.begin(BAUD, SERIAL_8N1, PIN_RX, PIN_TX);
  delay(50);

  // isACK = false deliberately. With ACK on, the library blocks on every single
  // command waiting for the module to answer (up to its timeout, ~500 ms by
  // default), and that would stall the 1 s countdown tick and the touch loop.
  dfp.begin(Serial2, /*isACK=*/false, /*doReset=*/true);

  // TRAP: begin()'s return value is useless here. Its last line is
  //   return (readType() == DFPlayerCardOnline) || ... || !isACK;
  // so with ACK off it returns true even with nothing wired up. The reset inside
  // it does wait up to 2 s for the module's card-online message and records it in
  // _handleType, so that is what gets asked instead.
  isOnline = (dfp.readType() == DFPlayerCardOnline);

  if (isOnline) setVolume(VOLUME_DEFAULT);
  else nextRetryMs = millis() + RETRY_MS;     // not given up on, see serviceOffline()
  return isOnline;
}

bool online() { return isOnline; }

void setVolume(uint8_t v) {
  if (v > VOLUME_MAX) v = VOLUME_MAX;
  lastVolume = v;
  if (isOnline) dfp.volume(v);
}

void play(uint8_t soundIndex, uint8_t seconds) {
  if (!isOnline || seconds == 0) return;
  if (soundIndex >= TRACK_COUNT) soundIndex = 0;

  // DO NOT re-send the volume here. Tried twice and reverted twice — at 22 on 2026-08-04
  // and at the 30 ceiling on 2026-08-18 — and BOTH made Test_ui quieter. The value is not
  // what matters; sending a volume command at all is what costs the loudness. See the
  // volume section of Project_Status.md.
  lastTrack = soundIndex + 1; // 0001.mp3 is sound index 0
  dfp.loop(lastTrack);
  isPlaying = true;
  stopAtMs = millis() + (uint32_t)seconds * 1000UL;
}

void stop() {
  if (!isPlaying) return;
  isPlaying = false;
  if (isOnline) dfp.stop();
}

bool playing() { return isPlaying; }

uint32_t restartCount() { return restarts; }

// Diagnostic (2026-09-24): the DFPlayer comes and goes on the station and nothing showed why.
// The module reports card and power-up events on its TX line even with ACK off; print them.
// "card online" or "card inserted" with nobody touching the card means the module restarted
// (a supply dip or a bad VCC/GND joint); "card removed" points at the SD card; silence while it
// is silent points at the speaker wires or the serial link.
//
// The restarts were never fully cured (2026-09-25: 4 in a 30-min full-load run, even with the
// DFPlayer cap and a charger), so a restart is recovered from rather than just reported:
//  - a restarted module is back at its own power-on volume (near max), which would undo the
//    station's cap of 20, so ours is put back;
//  - a tone it was playing has stopped, so if the station still wants sound it is started
//    again. The alarm then goes quiet only for the restart itself, not for good.
static void recoverFromRestart(const char* event) {
  restarts++;
  dfp.volume(lastVolume);
  if (isPlaying) {
    delay(100);               // keep the volume command apart from the play, as at start-up
    dfp.loop(lastTrack);
  }
  Serial.printf("[sound] DFPlayer: %s (module restarted?) - volume %u re-sent%s\n",
                event, lastVolume, isPlaying ? ", tone restarted" : "");
}

static void reportModuleEvents() {
  if (!isOnline || !dfp.available()) return;   // available() only reads what has arrived
  uint8_t type = dfp.readType();
  uint16_t value = dfp.read();
  switch (type) {
    case DFPlayerPlayFinished: return;          // every loop of the tone; not news
    case DFPlayerCardOnline:   recoverFromRestart("card online");   break;
    case DFPlayerCardInserted: recoverFromRestart("card inserted"); break;
    case DFPlayerCardRemoved:  Serial.println("[sound] DFPlayer: CARD REMOVED / lost"); break;
    case DFPlayerError:        Serial.printf("[sound] DFPlayer: error %u\n", value); break;
    case WrongStack:           Serial.println("[sound] DFPlayer: garbled message (serial link?)"); break;
    default:                   Serial.printf("[sound] DFPlayer: event %u value %u\n", type, value); break;
  }
}

// A module that missed begin()'s window is no longer given up on (2026-10-16, Dough: after a
// re-plug the speaker is sometimes dead until a second re-plug; rare, not reproducible on
// demand). begin() takes the FIRST message as the answer, so a module still reading its card
// when the reset arrived, or a half frame from the power-up, left isOnline false for good and
// every alarm silent until the next reboot. Now, while offline, poll() listens for a late
// "card online" and resets the module again every RETRY_MS (reset() only sends, it does not
// wait, so the UI never stalls). CODE ONLY until a silent start is seen to recover.
static void serviceOffline() {
  while (dfp.available()) {
    uint8_t type = dfp.readType();
    dfp.read();
    if (type == DFPlayerCardOnline || type == DFPlayerCardInserted) {
      isOnline = true;
      dfp.volume(lastVolume);   // a reset module is back at its own power-on volume
      Serial.printf("[sound] DFPlayer: online at %lu s (missed at start-up) - volume %u sent\n",
                    (unsigned long)(millis() / 1000), lastVolume);
      return;
    }
  }
  if ((int32_t)(millis() - nextRetryMs) >= 0) {
    nextRetryMs = millis() + RETRY_MS;
    dfp.reset();
    if (retriesPrinted < 3) {   // a station built without a DFPlayer would print this forever
      retriesPrinted++;
      Serial.println("[sound] DFPlayer offline - reset sent, waiting for card online");
    }
  }
}

void poll() {
  if (!isOnline) { serviceOffline(); return; }   // nothing can be playing while offline
  reportModuleEvents();
  if (!isPlaying) return;
  // Signed difference so this still works across the millis() rollover.
  if ((int32_t)(millis() - stopAtMs) >= 0) stop();
}

} // namespace Sound
