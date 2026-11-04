/*  Temperature logger for the report's heat tests (T2 warm-up, T3 hold, T4 hysteresis,
    T5 slot uniformity, T7 plate vs air). Written 2026-09-25, NOT compiled or run yet.
    Serial Monitor at 115200.

    It runs the station's real heat control (Heater: ON below SP-0.5, OFF above SP+0.5 —
    the same code Test_ui_fuji uses) and prints one CSV line per second:

        time_s,control_C,ssr,setpoint_C,probe2_C,probe3_C,...

    Save the output and turn it into graphs + numbers with tools/temp_log.py (which can also
    record the port straight to a file, so nothing has to be copied out of the Serial Monitor).
    Lines starting with '#' are comments; the Python script skips them.

    !!! MAINS. This switches the 220 V mat for real. Never leave it unattended. The mat's own
    thermostat is the only hardware over-temperature layer, and its trip point should be
    measured (Assembly checklist, step 4) before a long run.

    Build (hold BOOT while esptool prints "Connecting..."):
      arduino-cli compile --fqbn "esp32:esp32:esp32:UploadSpeed=115200" \
        --library "D:/COOPProject/Coding/Firmware" --upload -p COM3 .

    PROBES. Every DS18B20 on the GPIO 4 bus is read and logged (extra probes for T5/T7 go on the
    same three wires: 3.3 V, GND, data — one 4.7 kOhm pull-up for the whole bus). With ONE probe
    it is the control probe. With MORE than one, the sketch will not heat until CONTROL_ADDR
    below is set to the station's own probe: the bus order is set by the probes' serial numbers,
    so "the first one" could be a probe lying loose on the bench. At boot every address is
    printed; to find which is which, warm one probe in your hand and watch its column rise.

    SERIAL COMMANDS (type, then Enter):
      a number 40-50   new setpoint (clamped to the station's 40-50 lock)
      x                heat OFF and stay off (logging continues) — e.g. to log a cool-down
      h                heat back ON after x

    Safety, on top of Heater's own "unknown temperature = OFF":
      - control probe fails or goes stale (> 5 s) -> heat off, the line shows control_C = nan
      - control temperature above SP + 5 C, or above 55 C -> heat LATCHED off until reset
    The screen is not used: the panel stays blank/white while this runs. That is expected.

    XIAOMI Mi REFERENCE SENSOR (added 2026-10-01, T40 / T36). A Mi Temperature and Humidity
    Monitor 2 (LYWSD03MMC, stock firmware — the same model the BME department checks the box
    with) is read over Bluetooth and logged in two extra columns, mi_C and mi_RH (nan when there
    is no fresh reading). Needs the NimBLE-Arduino library (h2zero, version 2.x) installed.
      1. Leave MI_ADDR "" and flash: every few seconds the log prints "# Mi found: <address>
         signal <n> dBm" for each Mi it hears. Hold yours against the board — the strongest
         signal (closest to 0) is yours; the department's Mi may show up too.
      2. Copy that address into MI_ADDR and flash again. The log says "# Mi: connected".
    Close the Mi Home app on the phone while logging: the Mi takes one connection at a time.
    The Mi sends a reading every few seconds and reacts more slowly than the DS18B20, so mi_C
    repeats on consecutive lines and lags during warm-up — expected, not a fault.

    LOGGER-ONLY BOARD (LOG_ONLY = true, added 2026-10-01). For a SEPARATE ESP32 that only records
    — e.g. the old ESP32 with the test probes — while the station runs its normal firmware with
    its one probe. Nothing is switched: GPIO 16 is held LOW and the ssr column is logged as nan.
    control_C is then the REFERENCE probe: set CONTROL_ADDR to the test probe hung right beside
    the station's own probe (with no address set, probe 1 on the bus is used and the log says so).
    SETPOINT_C is only written into the log for the analysis — set it to the station's setpoint.
    The old ESP32 browned out when its radio started (2026-09-24), and reading the Mi uses the
    radio: if the log restarts with a "# ==== TEMPERATURE LOG" header right after "# Mi:
    connecting", set USE_MI = false and leave the Mi to the station board.
*/

#include <OneWire.h>
#include <DallasTemperature.h>
#include <heater.h>
#include <NimBLEDevice.h>

// ---- settings --------------------------------------------------------------------------
static constexpr float SETPOINT_C = 47.0f;       // 47 from 2026-10-02 (45-50 C rule); 50 for the dial 9 vs 5 runs (10-01)
// Station probe address, 16 hex digits as printed at boot, e.g. "28FF641E8C160344".
// Leave "" when only one probe is connected.
static const char* const CONTROL_ADDR = "";
static constexpr float OVERTEMP_ABOVE_SP_C = 5.0f;
static constexpr float OVERTEMP_ABS_C = 55.0f;
// true = record only, never switch the mat (separate logger board, see the top comment).
// 2026-10-12: set for the old ESP32 + 3 test probes (logger only, no radio). Set both back
// (false / true) before flashing this sketch to the station itself.
static constexpr bool LOG_ONLY = true;
// false = no Bluetooth at all (mi_C/mi_RH logged as nan) - for a board whose radio browns it out.
// 2026-10-30: true for the probe-height test (station probe only, arm 33 vs 70 mm; the Xiaomi on the holder at
// scope height is the reference the department itself uses). Set back to false if this board browns out.
static constexpr bool USE_MI = false;
// Xiaomi Mi Bluetooth address, e.g. "a4:c1:38:12:34:56". "" = scan mode (see the top comment).
static const char* const MI_ADDR = "a4:c1:38:ba:c6:ba";   // Dough's own Mi (found 2026-10-01)

// ---- hardware (Section 9) --------------------------------------------------------------
static constexpr uint8_t ONE_WIRE_PIN = 4;
static constexpr uint8_t MAX_PROBES = 8;
static constexpr uint32_t PERIOD_MS = 1000;       // 12-bit conversion ~750 ms fits inside
static constexpr uint32_t STALE_MS = 5000;        // same rule as TempSensor::STALE_MS

static OneWire oneWire(ONE_WIRE_PIN);
static DallasTemperature dallas(&oneWire);

static DeviceAddress addr[MAX_PROBES];
static uint8_t probeCount = 0;
static int8_t controlIdx = -1;                    // -1 = no control probe -> never heat

static float lastC[MAX_PROBES];
static bool lastOk[MAX_PROBES];
static float controlC = NAN;
static uint32_t controlGoodMs = 0;                // 0 = never had a good reading

static uint32_t startMs = 0, lastCycleMs = 0;
static bool heatWanted = true;                    // 'x' / 'h'
static bool overTempLatched = false;
static char cmd[8];
static uint8_t cmdLen = 0;

// ---- Xiaomi Mi reference sensor (Bluetooth) --------------------------------------------
// The stock firmware scrambles its broadcasts but answers a direct connection: subscribing to
// this characteristic gives a notification every few seconds — temperature (int16, 0.01 C),
// humidity (uint8, %), battery (uint16, mV), little-endian. Connecting can take many seconds,
// so it runs in its own task on core 0: loop()'s 1 s heat-control cycle is never held up.
static const NimBLEUUID MI_SERVICE("ebe0ccb0-7a0a-4b0c-8a1a-6ff2997da3a6");
static const NimBLEUUID MI_DATA("ebe0ccc1-7a0a-4b0c-8a1a-6ff2997da3a6");
static constexpr uint32_t MI_STALE_MS = 30000;    // no reading for 30 s -> logged as nan

static portMUX_TYPE miMux = portMUX_INITIALIZER_UNLOCKED;
static float miC = NAN, miRH = NAN;
static uint32_t miAtMs = 0;                       // 0 = never had a reading
// Messages from the Mi task are queued here and printed by loop() between CSV lines, so they
// never land in the middle of one.
static char miNotes[512];
static size_t miNotesLen = 0;

static void miNote(const char* fmt, ...) {
  char line[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  size_t n = strlen(line);
  portENTER_CRITICAL(&miMux);
  if (miNotesLen + n + 2 <= sizeof(miNotes)) {
    memcpy(miNotes + miNotesLen, line, n);
    miNotesLen += n;
    miNotes[miNotesLen++] = '\n';
    miNotes[miNotesLen] = '\0';
  }
  portEXIT_CRITICAL(&miMux);
}

static void printMiNotes() {
  char copy[sizeof(miNotes)];
  portENTER_CRITICAL(&miMux);
  memcpy(copy, miNotes, miNotesLen + 1);
  miNotesLen = 0;
  miNotes[0] = '\0';
  portEXIT_CRITICAL(&miMux);
  if (copy[0]) Serial.print(copy);
}

static void onMiData(NimBLERemoteCharacteristic*, uint8_t* d, size_t len, bool) {
  if (len < 3) return;
  float c = (int16_t)(d[0] | (d[1] << 8)) / 100.0f;
  float rh = d[2];
  uint32_t now = millis();
  portENTER_CRITICAL(&miMux);
  miC = c;
  miRH = rh;
  miAtMs = now ? now : 1;
  portEXIT_CRITICAL(&miMux);
}

static void miScan() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);   // the name comes in the scan response
  NimBLEScanResults res = scan->getResults(5000, false);
  for (int i = 0; i < res.getCount(); i++) {
    const NimBLEAdvertisedDevice* dev = res.getDevice(i);
    // By name, or by Xiaomi's MiBeacon service data (0xFE95) — the name only comes in the scan
    // response, which a busy room can miss (2026-10-01: the first scan found nothing by name).
    std::string name = dev->getName();
    bool xiaomi = name.find("LYWSD") != std::string::npos || name.find("MJWSD") != std::string::npos;
    for (uint8_t k = 0; !xiaomi && k < dev->getServiceDataCount(); k++) {
      xiaomi = dev->getServiceDataUUID(k) == NimBLEUUID((uint16_t)0xFE95);
    }
    if (xiaomi) {
      miNote("# Mi found: %s  signal %d dBm  name '%s'", dev->getAddress().toString().c_str(),
             dev->getRSSI(), name.c_str());
    }
  }
  scan->clearResults();
}

static void miTask(void*) {
  NimBLEDevice::init("");
  if (MI_ADDR[0] == '\0') {
    miNote("# Mi: MI_ADDR not set - scanning for LYWSD03MMC (logging continues without it)");
    for (;;) {
      miScan();
      vTaskDelay(pdMS_TO_TICKS(10000));
    }
  }
  NimBLEClient* client = NimBLEDevice::createClient();
  NimBLEAddress miAddr(std::string(MI_ADDR), BLE_ADDR_PUBLIC);
  for (;;) {
    if (!client->isConnected()) {
      miNote("# Mi: connecting to %s", MI_ADDR);
      bool ok = client->connect(miAddr);
      NimBLERemoteService* svc = ok ? client->getService(MI_SERVICE) : nullptr;
      NimBLERemoteCharacteristic* ch = svc ? svc->getCharacteristic(MI_DATA) : nullptr;
      if (ch && ch->canNotify() && ch->subscribe(true, onMiData)) {
        miNote("# Mi: connected");
      } else {
        miNote("# Mi: connect failed - in range? address right? Mi Home app closed? retrying");
        client->disconnect();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

static void addrToHex(const DeviceAddress a, char* out) {   // out: 17 chars
  for (uint8_t i = 0; i < 8; i++) sprintf(out + i * 2, "%02X", a[i]);
}

// Same rejection rule as TempSensor: -127 = disconnected, 85.00 = power-on value after a reset
// mid-conversion (loose data line or brown-out), not a real 85 C.
static bool plausible(float c) { return c != DEVICE_DISCONNECTED_C && c != 85.0f; }

static void findProbes() {
  dallas.begin();
  probeCount = 0;
  uint8_t n = dallas.getDeviceCount();
  for (uint8_t i = 0; i < n && probeCount < MAX_PROBES; i++) {
    if (!dallas.getAddress(addr[probeCount], i)) continue;
    dallas.setResolution(addr[probeCount], 12);
    lastOk[probeCount] = false;
    probeCount++;
  }
  dallas.setWaitForConversion(false);

  Serial.printf("# probes found: %u\n", probeCount);
  for (uint8_t i = 0; i < probeCount; i++) {
    char hex[17];
    addrToHex(addr[i], hex);
    bool isControl = CONTROL_ADDR[0] != '\0' && strcasecmp(hex, CONTROL_ADDR) == 0;
    if (isControl) controlIdx = (int8_t)i;
    Serial.printf("#   probe %u  address %s%s\n", i + 1, hex, isControl ? "  <- CONTROL" : "");
  }
  if (probeCount == 1 && CONTROL_ADDR[0] == '\0') controlIdx = 0;
  if (LOG_ONLY && controlIdx < 0 && probeCount > 0) {
    controlIdx = 0;
    Serial.println("# LOG_ONLY: reference = probe 1. Set CONTROL_ADDR to the probe beside the station's own probe.");
  }

  if (probeCount == 0) {
    Serial.println("# NO DS18B20 FOUND on GPIO 4 - check the 4.7k pull-up, the data wire, 3.3 V/GND.");
  } else if (controlIdx < 0 && !LOG_ONLY) {
    Serial.println("# NO CONTROL PROBE - with more than one probe, set CONTROL_ADDR to the station's");
    Serial.println("# probe and re-flash. Logging only; the heater stays OFF.");
  }
}

static void printHeader() {
  if (LOG_ONLY) {
    Serial.printf("# LOG_ONLY: recording only, nothing switched; station setpoint noted as %.1f C\n",
                  Heater::setpoint());
  } else {
    Serial.printf("# setpoint %.1f C, ON below %.1f, OFF above %.1f; latch off above %.1f C\n",
                  Heater::setpoint(), Heater::setpoint() - Heater::HYSTERESIS_C,
                  Heater::setpoint() + Heater::HYSTERESIS_C,
                  min(Heater::setpoint() + OVERTEMP_ABOVE_SP_C, OVERTEMP_ABS_C));
  }
  Serial.print("time_s,control_C,ssr,setpoint_C");
  uint8_t col = 2;
  for (uint8_t i = 0; i < probeCount; i++) {
    if ((int8_t)i == controlIdx) continue;
    Serial.printf(",probe%u_C", col++);
  }
  Serial.println(",mi_C,mi_RH");
}

static void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c != '\n' && c != '\r') {
      if (cmdLen < sizeof(cmd) - 1) cmd[cmdLen++] = c;
      continue;
    }
    if (cmdLen == 0) continue;
    cmd[cmdLen] = '\0';
    cmdLen = 0;
    if (cmd[0] == 'x' || cmd[0] == 'X') {
      heatWanted = false;
      Serial.println("# heat OFF by command (x); logging continues");
    } else if (cmd[0] == 'h' || cmd[0] == 'H') {
      heatWanted = true;
      Serial.println(overTempLatched ? "# heat ON requested, but the over-temperature latch holds - reset the board"
                                     : "# heat ON by command (h)");
    } else {
      float sp = Heater::setSetpoint(atof(cmd));
      Serial.printf("# new setpoint %.1f C (clamped to 40-50)\n", sp);
    }
  }
}

// Heater::update() turns the mat off whenever tempValid is false; every "not allowed" case is
// handled by forcing it off instead. Called every loop, and again in cycle() before the line is
// printed, so the logged ssr column matches the temperature on the same line.
static void applyControl(uint32_t now) {
  if (LOG_ONLY) return;   // Heater::begin() already holds GPIO 16 LOW
  bool controlValid = controlGoodMs != 0 && now - controlGoodMs <= STALE_MS;
  bool allowed = controlValid && heatWanted && !overTempLatched && controlIdx >= 0;
  if (allowed) Heater::update(controlC, true);
  else         Heater::forceOff();
}

// Once per PERIOD_MS: collect the conversion requested last cycle, log a line, request the next.
static void cycle(uint32_t now) {
  for (uint8_t i = 0; i < probeCount; i++) {
    float c = dallas.getTempC(addr[i]);
    lastOk[i] = plausible(c);
    if (lastOk[i]) lastC[i] = c;
  }
  if (controlIdx >= 0 && lastOk[controlIdx]) {
    controlC = lastC[controlIdx];
    controlGoodMs = now ? now : 1;
  }
  dallas.requestTemperatures();

  bool controlValid = controlGoodMs != 0 && now - controlGoodMs <= STALE_MS;
  if (!LOG_ONLY && controlValid && !overTempLatched &&
      (controlC > Heater::setpoint() + OVERTEMP_ABOVE_SP_C || controlC > OVERTEMP_ABS_C)) {
    overTempLatched = true;
    Heater::forceOff();
    Serial.printf("# OVER-TEMPERATURE %.2f C - heat LATCHED OFF until reset\n", controlC);
  }
  applyControl(now);

  printMiNotes();
  Serial.printf("%.1f,", (now - startMs) / 1000.0f);
  if (controlValid) Serial.printf("%.4f", controlC); else Serial.print("nan");
  if (LOG_ONLY) Serial.printf(",nan,%.1f", Heater::setpoint());
  else          Serial.printf(",%d,%.1f", Heater::isOn() ? 1 : 0, Heater::setpoint());
  for (uint8_t i = 0; i < probeCount; i++) {
    if ((int8_t)i == controlIdx) continue;
    if (lastOk[i]) Serial.printf(",%.4f", lastC[i]); else Serial.print(",nan");
  }
  portENTER_CRITICAL(&miMux);
  float mc = miC, mrh = miRH;
  uint32_t mAt = miAtMs;
  portEXIT_CRITICAL(&miMux);
  if (mAt != 0 && now - mAt <= MI_STALE_MS) Serial.printf(",%.2f,%.0f\n", mc, mrh);
  else Serial.println(",nan,nan");
}

void setup() {
  Heater::begin();   // FIRST: the mat must never be live during boot
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("# ==== TEMPERATURE LOG (Test_temp_log) ====");
  Heater::setSetpoint(SETPOINT_C);
  findProbes();
  printHeader();
  if (USE_MI) xTaskCreatePinnedToCore(miTask, "mi", 8192, nullptr, 1, nullptr, 0);
  dallas.requestTemperatures();
  startMs = lastCycleMs = millis();
}

void loop() {
  uint32_t now = millis();
  handleSerial();
  if (now - lastCycleMs >= PERIOD_MS) {
    lastCycleMs += PERIOD_MS;
    cycle(now);
  }
  applyControl(now);
}
