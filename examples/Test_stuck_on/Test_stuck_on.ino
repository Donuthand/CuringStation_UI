/*  Stuck-heater test (T8 at dial 7) — what the mat's own thermostat does when the controller fails.
    Serial Monitor at 115200, or record it:  python Coding/Firmware/tools/temp_log.py record COMx <csv>

    Build:
      arduino-cli compile --fqbn "esp32:esp32:esp32:UploadSpeed=115200" \
        --library "D:/COOPProject/Coding/Firmware" --upload -p COM3 .
      (hold the BOOT button while esptool prints its "Connecting..." dots)

    Written 2026-10-29 because the department staff want to know what protects the scopes if the
    station fails with the heater on. This sketch plays that fault: after "GO" it holds the SSR ON
    with no control at all, exactly like an SSR welded shut or firmware stuck with GPIO16 high.
    The mat's dial thermostat (dial 7) is then the only thing that can stop the heat; its pilot
    light going out is the moment it opens. Plan: Paper/Test_Plan_Dial7_Cutoff.md.

    It bypasses Heater::update() on purpose (that clamps to the 40-50 lock and would switch off at
    the setpoint), so it keeps its own safety stops, latched until reset:
      - air at the station probe >= ABORT_AIR_C   (well below where the PETG parts and the
                                                   acrylic hood soften, ~80 C)
      - no valid reading for SENSOR_LOST_MS       (same 10 s as the station's sensor fault)
      - MAX_RUN_MIN reached
      - any key typed on the Serial Monitor       (the operator's stop)
    None of these replaces the person standing at the plug for the whole run.
*/

#include <temp_sensor.h>
#include <heater.h>

static constexpr float ABORT_AIR_C = 65.0f;
static constexpr uint32_t SENSOR_LOST_MS = 10000;
static constexpr uint32_t MAX_RUN_MIN = 90;
static constexpr uint32_t PRINT_INTERVAL_MS = 1000;

static bool armed = false;      // waiting for "GO"
static bool running = false;    // SSR held on
static bool stopped = false;    // latched off until reset
static uint32_t startMs = 0, lastPrintMs = 0, lastValidMs = 0;
static String rx;

static void ssr(bool on) { digitalWrite(Heater::SSR_PIN, on ? HIGH : LOW); }

static void stopRun(const char* why) {
  ssr(false);
  running = false;
  stopped = true;
  Serial.print("# STOPPED: "); Serial.print(why);
  Serial.println(" -- SSR OFF, latched until reset. Unplug the station when done.");
}

void setup() {
  // First, before anything slow: the mat must never be live during boot.
  Heater::begin();

  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("# ########  STUCK-HEATER TEST (T8, dial 7)  ########");
  Serial.println("# The SSR will be held ON with NO temperature control.");
  Serial.print("# Own stops: air >= "); Serial.print(ABORT_AIR_C, 1);
  Serial.print(" C, probe lost 10 s, "); Serial.print(MAX_RUN_MIN);
  Serial.println(" min, or any key. Stay at the plug the whole time.");
  if (!TempSensor::begin()) {
    Serial.println("# No DS18B20 on GPIO4 -- the test will not start without the probe.");
  }
  Serial.println("# Check: dial at 7, lid closed, PETG parts out (plan step 2). Type GO and Enter to start.");
  armed = true;
}

void loop() {
  TempSensor::poll();
  float tempC = 0.0f;
  bool valid = TempSensor::read(tempC);
  if (valid) lastValidMs = millis();

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (running) {
      // The line ending after GO arrives here too, so only a real character stops the run.
      if (c != '\n' && c != '\r') { stopRun("operator key"); rx = ""; break; }
      continue;
    }
    if (c == '\n' || c == '\r') {
      rx.trim();
      if (armed && !stopped && rx.equalsIgnoreCase("GO")) {
        if (!valid) {
          Serial.println("# No valid probe reading -- not starting.");
        } else {
          armed = false;
          running = true;
          startMs = millis();
          lastValidMs = startMs;
          ssr(true);
          Serial.println("# GO: SSR ON, no control from here.");
          Serial.println("time_s,control_C,ssr");
        }
      }
      rx = "";
    } else {
      rx += c;
    }
  }

  if (running) {
    if (valid && tempC >= ABORT_AIR_C) stopRun("air reached the abort limit");
    else if (millis() - lastValidMs >= SENSOR_LOST_MS) stopRun("no probe reading for 10 s");
    else if (millis() - startMs >= MAX_RUN_MIN * 60UL * 1000UL) stopRun("time limit");
  }

  // Before GO: the air every 10 s as a comment line, so the cool-down can be watched and the start
  // temperature is on record without adding rows to the CSV (added 2026-10-29, between the dial runs).
  if (armed && !running && !stopped && millis() - lastPrintMs >= 10000) {
    lastPrintMs = millis();
    Serial.print("# waiting for GO, air ");
    if (valid) Serial.print(tempC, 2); else Serial.print("nan");
    Serial.println(" C, heater off");
  }

  if ((running || stopped) && millis() - lastPrintMs >= PRINT_INTERVAL_MS) {
    lastPrintMs = millis();
    Serial.print((millis() - startMs) / 1000.0f, 1); Serial.print(',');
    if (valid) Serial.print(tempC, 4); else Serial.print("nan");
    Serial.print(',');
    Serial.println(running ? 1 : 0);
  }
}
