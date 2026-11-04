/*  UI bring-up — STEPS A-F: the whole screen, saved state across a power cut, and
    the audible alarm. Serial Monitor at 115200.

    Build (hold BOOT while esptool prints its "Connecting..." dots):
      arduino-cli compile --fqbn "esp32:esp32:esp32:UploadSpeed=115200" \
        --library "D:/COOPProject/Coding/Firmware" --upload -p COM3 .

    What works:
      - the main screen, laid out like epoxy_curing_station_UI.html
      - six slots counting down once a second, progress bars changing colour
      - the start-slot flow, the cancel confirmation, the finished-cure alarm
      - the menu, both settings screens, the temp log and user management
      - the screen blanks after the sleep timeout and any touch wakes it
      - STEP E: users, settings and slots survive a power cut, and cures that come
        back still running raise the "power was interrupted" warning first
      - STEP F: a finished cure plays its own tone through the DFPlayer for
        alarmDurationSec, and Stop silences it
      - the physical 4x3 keypad drives the same screens as touch — see UI::handleKey
        in ui.h for the map and the three places it differs from the mockup
      - (DISCONNECTED 2026-09-15) the ESP-NOW alarm-stop button. The radio start
        browns out this board (E BOD at WiFi.mode, unresolved — see Project_Status.md),
        so RemoteStop is not started here. remote_stop.h/.cpp are kept; put the three
        RemoteStop lines back only after Test_radio_power passes.

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

#include <heater.h>
#include <matrix_keypad.h>
#include <persist.h>
#include <sound.h>
#include <station.h>
#include <temp_sensor.h>
#include <ui.h>

// Three minutes is a long time to wait when testing the blanking. Set this to 15000
// (15 s) to check it quickly, then put it back to UI::SLEEP_AFTER_MS.
static const uint32_t SLEEP_TIMEOUT_MS = UI::SLEEP_AFTER_MS;

static uint32_t lastTickMs = 0;

// Which slot the speaker is currently announcing, -1 for silence.
//
// STEP F glue. `Sound` plays what it is told and `Station` has no hardware, so the
// decision of WHEN to make noise belongs neither in one nor the other — it lives
// here, the same way the temperature reaches the header through UI::setHeader().
static int8_t soundingSlot = -1;

static void serviceAlarmSound() {
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
    Serial.println("[setup] no DS18B20 on GPIO4 - header shows --, heat stays off.");
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

    Station::startSlot(0, "Dough",  0, 60);
    Station::startSlot(1, "User B", 2, 45);
    Station::startSlot(2, "User C", 3, 1);   // finishes in a minute, to show DONE
    // slots 3-5 stay empty
  }

  KeypadDriver::begin();

  UI::begin();
  UI::setSleepTimeout(SLEEP_TIMEOUT_MS);

  // The countdowns are short by however long the power was off and nothing here can
  // know by how much, so the operator gets told rather than quietly resumed.
  if (restored.running) UI::showPowerLossWarning();

  Serial.print("Screen blanks after ");
  Serial.print(SLEEP_TIMEOUT_MS / 1000);
  Serial.println(" s with no touch. Any touch wakes it.");
}

void loop() {
  UI::service();
  UI::handleKey(KeypadDriver::poll()); // poll() returns 0 when nothing is pressed
  Persist::service();
  Sound::poll();
  serviceAlarmSound();
  TempSensor::poll();   // non-blocking: requests a conversion, collects it ~800 ms later

  if (millis() - lastTickMs >= 1000) {
    lastTickMs += 1000;
    Station::tickOneSecond();

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

    UI::setHeader(tempC, Heater::isOn(), tempValid);

    // Only log real readings. Feeding the log a 0.0 while the probe is quiet would
    // draw a cliff to the bottom of the chart, which reads as a temperature crash
    // rather than as missing data. A gap in the trace is the honest picture.
    if (tempValid) Station::sampleTemperature(tempC);

    // A cure that finishes while the screen is blank has to bring the screen back —
    // otherwise the alarm is invisible until somebody happens to touch it.
    if (Station::hasPendingAlarm() && UI::asleep()) {
      Serial.println("[UI] alarm pending, waking screen");
      UI::wake();
    }
  }
}
