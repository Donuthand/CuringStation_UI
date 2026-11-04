/*  UI bring-up — STEPS A-F: the whole screen, saved state across a power cut, and
    the audible alarm. Serial Monitor at 115200.

    Build (hold BOOT while esptool prints its "Connecting..." dots):
      arduino-cli compile --fqbn "esp32:esp32:esp32:UploadSpeed=115200" \
        --library "D:/COOPProject/Coding/Firmware" --upload -p COM3 .

    What works:
      - every screen in the Concept B design (new_ui_try/5_fujifilm_concept_B_bigtext_try.html):
        wordmark header, six fixed cards, footer, green completion overlay, and the dark
        subpages with the optional Curing position step
      - six slots counting down once a second
      - the start-slot flow, the cancel confirmation, the finished-cure alarm
      - the menu, both settings screens, the temp log and user management
      - the screen blanks after the sleep timeout and any touch wakes it
      - STEP E: users, settings and slots survive a power cut, and cures that come
        back still running raise the "power was interrupted" warning first
      - STEP F: a finished cure plays its own tone through the DFPlayer for
        alarmDurationSec, and Stop silences it
      - the physical 4x3 keypad drives the same screens as touch — see UIFuji::handleKey
        in ui.h for the map and the three places it differs from the mockup
      - (RECONNECTED 2026-09-24) the ESP-NOW alarm-stop button: a press acks the oldest
        alarm, like Stop. Disconnected 2026-09-15 because the radio start browned out
        the old board; back on now that the new ESP32 + protoboard caps passed
        Test_radio_power. Serial shows "[RemoteStop] heard packet #N" per press.

    To test the keypad: on the main screen press # (starts a cure) and 1-6 (acts on
    that slot). In the start flow, digits pick the list row, # confirms, * goes back.
    Letters still need touch — a numeric pad cannot type a name.

    To re-test step E: let a cure run past one 30 s save, pull the USB, plug it back
    in. The warning should list that cure with roughly the time it had at the save.

    To test step F: start a 1-minute cure and wait. Run Test_sound first if the
    module has never been checked — debugging audio behind the whole UI is harder
    than debugging it on its own.

    Temperature is REAL as of 2026-08-10 — the DS18B20 on GPIO4 feeds the header and
    the temp log. Nothing is wired to GPIO16 yet, so Heater::update() toggles the pin
    into open air; that is deliberate and harmless, and it is what makes the HEAT
    badge show the true hysteresis state rather than a guess. Build Order step 5
    (transistor + SSR) changes the wiring, not this sketch.
*/

// Test_ui_fuji — the same sketch as Test_ui, driving UIFuji (src/ui_fuji.*): the Concept B UI
// on the CAPACITIVE module (FT6336U touch on SDA 15 / SCL 22, inversion off). Flash
// Test_ui for the resistive panel instead.

#include <heater.h>
#include <matrix_keypad.h>
#include <persist.h>
#include <remote_stop.h>
#include <sound.h>
#include <station.h>
#include <temp_sensor.h>
#include <ui_fuji.h>

// Three minutes is a long time to wait when testing the blanking. Set this to 15000
// (15 s) to check it quickly, then put it back to UIFuji::SLEEP_AFTER_MS.
static const uint32_t SLEEP_TIMEOUT_MS = UIFuji::SLEEP_AFTER_MS;

static uint32_t lastTickMs = 0;

// Which slot the speaker is currently announcing, -1 for silence.
//
// STEP F glue. `Sound` plays what it is told and `Station` has no hardware, so the
// decision of WHEN to make noise belongs neither in one nor the other — it lives
// here, the same way the temperature reaches the header through UIFuji::setHeader().
static int8_t soundingSlot = -1;

// Over-temperature alarm (Dough, 2026-10-01): the air above UIFuji::OVERTEMP_C (55 C) for
// UIFuji::OVERTEMP_HOLD_MS (5 min) in a row -> red warning + a tone that repeats until silenced.
// Below 55 C it clears by itself. It cannot switch the mat off — if it fires, the SSR or the
// firmware has already failed to — so it tells a person to pull the plug.
static uint32_t overSinceMs = 0;     // when the air went above 55 C; 0 = not above
static bool overTempAlarm = false;
// Sensor fault (Dough, 2026-10-06, after T9): no valid reading for UIFuji::SENSORFAULT_HOLD_MS
// in a row -> red warning + the same repeating tone. The heater is already off by then; this is
// so nobody trusts a cure that finishes with no heat. Clears by itself when the reading returns.
static uint32_t lostSinceMs = 0;     // when the reading was first missing; 0 = reading OK
static bool sensorFault = false;
static bool warningSounding = false; // either red warning's tone is playing
// Tone 1 for now: all ten tones are user tones, so it is not unique. A dedicated 0011.mp3
// would need Sound::TRACK_COUNT raised and the file on the card.
static constexpr uint8_t OVERTEMP_TONE = 0;

static void serviceAlarmSound() {
  // The red warnings (over-temperature, sensor fault) outrank finished cures: they repeat until
  // silenced, then the cure alarm (if any) is announced again below because soundingSlot was reset.
  if ((overTempAlarm && !UIFuji::overTempSilenced()) ||
      (sensorFault && !UIFuji::sensorFaultSilenced())) {
    if (!warningSounding || !Sound::playing()) {
      Sound::play(OVERTEMP_TONE, 60);
      warningSounding = true;
      soundingSlot = -1;
    }
    return;
  }
  if (warningSounding) {
    Sound::stop();
    warningSounding = false;
  }

  int8_t slot = Station::pendingAlarmSlot();   // oldest unacknowledged, -1 if none

  if (slot < 0) {
    // Everything has been acknowledged. Covers the last Stop on the popup and also
    // a cure cancelled in its final second, which drops out of the queue.
    if (soundingSlot >= 0) {
      Sound::stop();
      soundingSlot = -1;
    }
    return;
  }

  // A different cure has reached the head of the queue, so announce that one with
  // its own tone. Note this deliberately does NOT re-trigger when the duration
  // expires: each cure sounds once for alarmDurationSec and then goes quiet, while
  // its card stays on screen until somebody taps Stop. That is what the popup's
  // "sounding for 15s" line has been promising since step C.
  if (slot != soundingSlot) {
    soundingSlot = slot;
    Sound::play(Station::slot(slot).sound, Station::alarmDurationSec());
  }
}

void setup() {
  // First line, before anything slow: the mat must never be live during boot, and
  // display init plus the DFPlayer handshake take seconds between them.
  Heater::begin();

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== UI STEPS A-E ===");

  Station::begin();

  if (!TempSensor::begin()) {
    Serial.println("[setup] no DS18B20 on GPIO4 - header shows --, heat stays off, sensor fault in 10 s.");
  }

  // Before the display, because begin() spends up to ~2.2 s on the reset handshake
  // and there is nothing on screen yet for it to hold up. A missing module only
  // costs the alarm its noise, so this is a warning, not a failure.
  if (!Sound::begin()) {
    Serial.println("[setup] no DFPlayer - alarms will be silent. Run Test_sound to fix.");
  }

  // Must come after Station::begin(), which wipes everything.
  Persist::Restored restored = Persist::begin();

  // Seed the mockup's demo data only on a virgin board. Once anything has been
  // saved, re-seeding would add the same five users again on every single boot.
  if (!restored.config) {
    Serial.println("[setup] nothing saved yet, seeding demo data");
    Station::setSetpointC(45);
    Station::setAlarmDurationSec(15);

    Station::addUser("Dough",   0);
    Station::addUser("User A", 1);
    Station::addUser("User B",  2);
    Station::addUser("User C",  3);
    Station::addUser("User D",  4);

    // No demo cures: a new installation starts with all six slots empty (handoff 11,
    // 2026-09-23). Start a 1-minute cure by hand to see a card go green.
  }

  KeypadDriver::begin();

  UIFuji::begin();
  UIFuji::setSleepTimeout(SLEEP_TIMEOUT_MS);

  // The countdowns are short by however long the power was off and nothing here can
  // know by how much, so the operator gets told rather than quietly resumed.
  if (restored.running) UIFuji::showPowerLossWarning();

  // Last, so the radio starts with the screen and DFPlayer already drawing current —
  // the worst case the brownout test needs to cover. A failure only costs the button.
  RemoteStop::begin(UIFuji::remoteStopAlarm);

  Serial.print("Screen sleeps after ");
  Serial.print(SLEEP_TIMEOUT_MS / 1000);
  Serial.println(" s with no cure running. Any touch or key wakes it.");

  // Once-a-second log for long runs (T26): same first four columns as Test_temp_log, so
  // tools/temp_log.py records and plots it as is; it skips the [..] event lines in between.
  Serial.println("time_s,control_C,ssr,setpoint_C,free_heap_B,min_free_heap_B");
}

void loop() {
  UIFuji::service();
  UIFuji::handleKey(KeypadDriver::poll()); // poll() returns 0 when nothing is pressed
  Persist::service();
  Sound::poll();
  RemoteStop::poll();   // before serviceAlarmSound(), so a press silences in the same loop
  serviceAlarmSound();

  // Tone preview (play button on the tone lists, 2026-09-24): a few seconds of that tone at
  // the current volume. A real alarm always wins — none plays while one is pending, and
  // serviceAlarmSound() takes the speaker over if a cure finishes mid-preview.
  int8_t preview = UIFuji::takeTonePreview();
  if (preview >= 0 && !Station::hasPendingAlarm() && !overTempAlarm && !sensorFault) Sound::play((uint8_t)preview, 3);
  TempSensor::poll();   // non-blocking: requests a conversion, collects it ~800 ms later

  if (millis() - lastTickMs >= 1000) {
    lastTickMs += 1000;
    // Held while the power-loss page waits for an answer: the restored times stay exactly
    // as listed until 1 (keep) or 2 (clear) is pressed. The heater below keeps running.
    if (!UIFuji::holdingCountdowns()) Station::tickOneSecond();

    float tempC = 0.0f;
    bool tempValid = TempSensor::read(tempC);

    // Station owns the setpoint (the menu edits it, Persist saves it) and Heater
    // keeps its own clamped copy, so they have to be resynced. Done here every
    // second rather than at the menu's call sites: a setting changed anywhere —
    // including restored out of NVS at boot — cannot then be missed.
    Heater::setSetpoint((float)Station::setpointC());
    // Switched off from the menu — by hand or by the off timer, which Station ran out
    // in tickOneSecond() just above. The mat stays cold whatever the temperature
    // until someone turns it back on.
    if (Station::heatEnabled()) Heater::update(tempC, tempValid);
    else                        Heater::forceOff();

    // Logs every SSR switch with the temperature that caused it, so the hysteresis points
    // (SP-0.5 on, SP+0.5 off) can be read off Serial instead of watching the SSR LED.
    static bool lastHeatOn = false;
    if (Heater::isOn() != lastHeatOn) {
      lastHeatOn = Heater::isOn();
      Serial.printf("[heater] SSR %s at %.2f C (setpoint %d C)\n",
                    lastHeatOn ? "ON " : "OFF", tempC, Station::setpointC());
    }

    // The log line (header printed at the end of setup). A falling free heap over hours = a leak.
    Serial.printf("%.1f,", millis() / 1000.0f);
    if (tempValid) Serial.printf("%.4f", tempC); else Serial.print("nan");
    Serial.printf(",%d,%.1f,%u,%u\n", Heater::isOn() ? 1 : 0, Heater::setpoint(),
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());

    // Alarm volume (Settings > Alarm settings, 2026-09-24): sent only when it differs from
    // what the module was last given, and only while it is silent. August's four attempts
    // found that a volume command sent right alongside play() costs loudness, so this never
    // shares a moment with serviceAlarmSound(). Sound::begin() already sent the default.
    static_assert(Station::ALARM_VOL_DEFAULT == Sound::VOLUME_DEFAULT,
                  "a station that never touched the setting must send no extra volume command");
    static uint8_t sentVolume = Sound::VOLUME_DEFAULT;
    if (Station::alarmVolume() != sentVolume && !Sound::playing()) {
      sentVolume = Station::alarmVolume();
      Sound::setVolume(sentVolume);
      Serial.printf("[sound] volume set to %u\n", sentVolume);
    }

    UIFuji::setHeader(tempC, Heater::isOn(), tempValid);

    // Over-temperature: needs 5 unbroken minutes above 55 C, so the normal overshoot (~53 C)
    // never trips it. A lost reading restarts the 5 minutes but does not clear a raised alarm.
    if (tempValid && tempC > UIFuji::OVERTEMP_C) {
      if (overSinceMs == 0) overSinceMs = millis();
      if (!overTempAlarm && millis() - overSinceMs >= UIFuji::OVERTEMP_HOLD_MS) {
        overTempAlarm = true;
        Serial.printf("[overtemp] ALARM: %.2f C above %.0f C for 5 min\n", tempC, UIFuji::OVERTEMP_C);
      }
    } else {
      overSinceMs = 0;
      if (overTempAlarm && tempValid) {
        overTempAlarm = false;
        Serial.printf("[overtemp] cleared at %.2f C\n", tempC);
      }
    }
    UIFuji::setOverTemp(overTempAlarm, tempC);

    // Sensor fault: the reading itself already counts as lost after TempSensor::STALE_MS (5 s),
    // so the warning comes ~15 s after the probe stops answering. Not raised on top of the
    // over-temperature warning, which already says to unplug the mat.
    if (tempValid || overTempAlarm) {
      lostSinceMs = 0;
      if (sensorFault) {
        sensorFault = false;
        Serial.println(tempValid ? "[sensor] reading back, fault cleared"
                                 : "[sensor] fault handed over to the over-temperature warning");
      }
    } else {
      if (lostSinceMs == 0) lostSinceMs = millis();
      if (!sensorFault && millis() - lostSinceMs >= UIFuji::SENSORFAULT_HOLD_MS) {
        sensorFault = true;
        Serial.printf("[sensor] FAULT: no reading for %lu s\n",
                      (unsigned long)(UIFuji::SENSORFAULT_HOLD_MS / 1000UL));
      }
    }
    UIFuji::setSensorFault(sensorFault);

    // A missing reading goes in as NAN, which the chart draws as a gap. Feeding it a 0.0
    // would draw a cliff to the bottom of the chart, which reads as a temperature crash
    // rather than as missing data; skipping it would join the trace across the outage.
    Station::sampleTemperature(tempValid ? tempC : NAN);

    // A cure that finishes while the screen is blank has to bring the screen back —
    // otherwise the alarm is invisible until somebody happens to touch it.
    if (Station::hasPendingAlarm() && UIFuji::asleep()) {
      Serial.println("[UI] alarm pending, waking screen");
      UIFuji::wake();
    }
  }
}
