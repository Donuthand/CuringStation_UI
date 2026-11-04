/*  Full-load stress test — 30 minutes of every load at once, to prove the station's power
    holds before real use (Dough, 2026-09-25). Serial Monitor at 115200.

    !!! UNPLUG THE 220 V HEATING MAT FIRST. !!!  The SSR is switched on and off every 5 s for
    the whole test. With mains connected that is 30 minutes of heating with nobody watching
    the temperature. The sketch waits for you to type  y  before it starts, as a reminder.

    Build (hold BOOT while esptool prints its "Connecting..." dots):
      arduino-cli compile --fqbn "esp32:esp32:esp32:UploadSpeed=115200" \
        --library "D:/COOPProject/Coding/Firmware" --upload -p COM3 .

    Loads, all at once:
      - ESP-NOW radio on, counting every C3 button press
      - DFPlayer playing non-stop at volume 20 (the station's cap), cycling through all 5 tones
      - screen repainting full-screen colours non-stop (max SPI traffic, backlight on)
      - SSR toggling every 5 s (LED only, mains unplugged)
      - DS18B20 read every second

    Read the result:
      - A line every 10 s. If the ESP32 restarts at any point, the next boot prints
        "last reset: BROWNOUT" (or PANIC / WATCHDOG) at the top, so an unattended run is still
        readable afterwards.
      - "[sound] DFPlayer: card online" during the test = the DFPlayer restarted (supply dip or
        a bad VCC/GND joint), even if the ESP32 stayed up.
      - PASS = reaches "=== TEST COMPLETE ===" with no restart and no DFPlayer restart, every C3
        press counted, and the regulator and DFPlayer chip warm, not hot (touch-check at 10 min).

    Off a phone charger (no Serial Monitor, 2026-09-25): touch the screen to start instead of
    typing y. The start screen shows the last reset reason, so if the ESP32 restarts mid-test
    you come back to it saying BROWNOUT etc. At the end the screen shows the result: DFPlayer
    restarts, C3 presses, tone starts.
*/

#include <TFT_eSPI.h>
#include <esp_system.h>

#include <ctp_touch.h>
#include <heater.h>
#include <remote_stop.h>
#include <sound.h>
#include <temp_sensor.h>

static constexpr uint32_t TEST_MS   = 30UL * 60UL * 1000UL;
static constexpr uint8_t  VOLUME    = 20;     // Station::ALARM_VOL_MAX
static constexpr uint8_t  TONE_SEC  = 250;    // Sound::play() takes up to 255 s per call
static constexpr uint32_t SSR_MS    = 5000;
static constexpr uint32_t REPORT_MS = 10000;

static TFT_eSPI tft;

static uint32_t startMs = 0, lastReportMs = 0, lastSsrMs = 0;
static uint32_t presses = 0, toneStarts = 0, frames = 0, framesAtReport = 0;
static uint8_t toneIdx = 0;
static bool ssrOn = false, finished = false;

// RemoteStop calls this from RemoteStop::poll() on the main loop, not from the radio task.
static void onStop() {
  presses++;
  Serial.printf("[load] C3 press #%lu received\n", (unsigned long)presses);
}

static const char* resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power on (normal)";
    case ESP_RST_EXT:      return "external pin";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "PANIC (crash)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "WATCHDOG";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    default:               return "other";
  }
}

// Start screen: the mains warning, plus the last reset so a mid-test restart is visible
// without Serial.
static void drawStartScreen(const char* lastReset) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("FULL LOAD TEST - 30 min", 10, 10, 4);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.drawString("220 V MAT UNPLUGGED?", 10, 70, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Touch the screen to start", 10, 130, 4);
  tft.drawString("(or type y in Serial Monitor)", 10, 165, 2);
  tft.drawString(String("last reset: ") + lastReset, 10, 250, 4);
}

// Waits for y on Serial or a finger held on the screen. The touch must be seen on
// 5 polls in a row, so one bad reading from the chip cannot start the test.
static void waitForStart(bool touchOk) {
  uint8_t held = 0;
  for (;;) {
    int c = Serial.read();
    if (c == 'y' || c == 'Y') return;
    int16_t x, y;
    held = (touchOk && CtpTouch::read(x, y)) ? held + 1 : 0;
    if (held >= 5) return;
    delay(20);
  }
}

static void drawResultScreen() {
  uint32_t dfpRestarts = Sound::restartCount();
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("TEST COMPLETE - 30:00", 10, 10, 4);
  tft.drawString("ESP32 restarts: 0", 10, 60, 4);   // a restart would have ended the run
  tft.setTextColor(dfpRestarts == 0 ? TFT_GREEN : TFT_RED, TFT_BLACK);
  tft.drawString(String("DFPlayer restarts: ") + dfpRestarts, 10, 100, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String("C3 presses: ") + presses, 10, 140, 4);
  tft.drawString(String("tone starts: ") + toneStarts + " (expect 8)", 10, 180, 4);
  tft.drawString("Check: presses = your tally, chips not hot", 10, 240, 2);
}

static void printMmSs(uint32_t ms) {
  uint32_t s = ms / 1000;
  Serial.printf("%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

void setup() {
  Heater::begin();   // SSR pin LOW before anything slow, same as the real sketch

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== FULL LOAD TEST (30 min) ===");
  const char* lastReset = resetName(esp_reset_reason());
  Serial.printf("last reset: %s\n", lastReset);

  tft.init();
  tft.setRotation(1);         // CtpTouch::read() maps rotation 1 only
  tft.invertDisplay(false);   // capacitive module, as in UIFuji::begin()
  bool touchOk = CtpTouch::begin();   // after tft.init(), see ctp_touch.h
  if (!touchOk) Serial.println("[load] no touch chip - type y to start");

  drawStartScreen(lastReset);
  Serial.println("220 V heating mat UNPLUGGED? Type y and press Enter (or touch the screen) to start.");
  waitForStart(touchOk);
  tft.fillScreen(TFT_BLACK);

  if (!TempSensor::begin()) Serial.println("[load] no DS18B20 - carrying on without it");
  if (!Sound::begin())      Serial.println("[load] no DFPlayer - the sound load is MISSING");
  Sound::setVolume(VOLUME);
  delay(100);                 // keep the volume command apart from the first play
  RemoteStop::begin(onStop);  // radio last, with everything else already drawing current

  startMs = lastReportMs = lastSsrMs = millis();
  Serial.println("[load] started - press the C3 button now and then");
}

void loop() {
  uint32_t now = millis();
  Sound::poll();          // also prints any DFPlayer events
  RemoteStop::poll();
  TempSensor::poll();

  if (finished) return;   // radio keeps counting presses; everything else is off

  if (now - startMs >= TEST_MS) {
    finished = true;
    Sound::stop();
    Heater::forceOff();
    drawResultScreen();
    Serial.println("=== TEST COMPLETE ===");
    Serial.printf("ran 30:00 with no ESP32 restart | C3 presses %lu | tone starts %lu (expect 8)\n",
                  (unsigned long)presses, (unsigned long)toneStarts);
    Serial.printf("DFPlayer restarts: %lu\n", (unsigned long)Sound::restartCount());
    Serial.println("PASS if: no 'DFPlayer: card online' lines above, every press counted, chips not hot.");
    return;
  }

  if (!Sound::playing()) {
    Sound::play(toneIdx, TONE_SEC);
    toneStarts++;
    toneIdx = (toneIdx + 1) % Sound::TRACK_COUNT;
  }

  if (now - lastSsrMs >= SSR_MS) {
    lastSsrMs = now;
    ssrOn = !ssrOn;
    digitalWrite(Heater::SSR_PIN, ssrOn ? HIGH : LOW);
  }

  static const uint16_t COLOURS[4] = { TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE };
  tft.fillScreen(COLOURS[frames % 4]);
  frames++;

  if (now - lastReportMs >= REPORT_MS) {
    float c = 0.0f;
    bool ok = TempSensor::read(c);
    Serial.print("[load] ");
    printMmSs(now - startMs);
    Serial.printf(" | presses %lu | tone starts %lu | temp %s%.1f C | SSR %s | %.1f fps\n",
                  (unsigned long)presses, (unsigned long)toneStarts, ok ? "" : "?", c,
                  ssrOn ? "ON " : "off",
                  (frames - framesAtReport) * 1000.0f / (float)(now - lastReportMs));
    lastReportMs = now;
    framesAtReport = frames;
  }
}
