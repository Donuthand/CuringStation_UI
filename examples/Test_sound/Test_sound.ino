/*  Bring-up sketch for the DFPlayer Mini — Build Order step 4.
    "Add DFPlayer + speaker -> confirm MP3 plays over the new GPIO 17/35 pins."
    Serial Monitor at 115200.

    Build (hold BOOT while esptool prints its "Connecting..." dots):
      arduino-cli compile --fqbn "esp32:esp32:esp32:UploadSpeed=115200" \
        --library "D:/COOPProject/Coding/Firmware" --upload -p COM3 .

    Wiring (Section 10):
      VCC   -> 5V / VIN      NOT 3.3V
      GND   -> GND
      TX    -> GPIO35        direct
      RX    -> 1 kOhm -> GPIO17
      SPK_1 -> speaker +
      SPK_2 -> speaker -

    The microSD must be FAT32 with 0001.mp3 .. 0010.mp3 in the root, in the order of
    Station::SOUND_NAMES: Tone 1 .. Tone 10. Copy them one at a time in
    that order — the DFPlayer plays by index in the card's file-allocation order, not
    by name, so bulk-copying the files at once can leave them in the wrong order.

    Nothing else is wired here: no display, no probe, no SSR. If a track plays, this
    subsystem is done and step F can fold Sound into the UI.
*/

#include <sound.h>

// Straight from Station::SOUND_NAMES, duplicated on purpose so this sketch stays
// standalone and does not drag the data model in just to print the labels.
static const char* const TONE_NAMES[Sound::TRACK_COUNT] = {
  "Tone 1", "Tone 2", "Tone 3", "Tone 4", "Tone 5", "Tone 6", "Tone 7", "Tone 8", "Tone 9", "Tone 10"
};

static constexpr uint8_t PLAY_SECONDS = 4;
static constexpr uint32_t GAP_MS = 1500;

static uint8_t track = 0;
static uint32_t nextStartMs = 0;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("########  DFPLAYER MINI CHECK  ########");
  Serial.println("Serial2: RX=GPIO35 (DFPlayer TX), TX=GPIO17 (via 1k to DFPlayer RX)");

  if (!Sound::begin()) {
    Serial.println(">> No DFPlayer answered. Check, in this order:");
    Serial.println("   1. the microSD is in, FAT32, with 0001.mp3 .. 0005.mp3 in the root");
    Serial.println("   2. VCC on 5V/VIN -- 3.3V is not enough for this module");
    Serial.println("   3. DFPlayer TX -> GPIO35, and the 1k from GPIO17 -> DFPlayer RX");
    Serial.println("   4. GND shared with the ESP32");
    Serial.println("   The sketch keeps running so the wiring can be fixed live, but");
    Serial.println("   nothing will play until it is reset with the module present.");
    return;
  }

  Serial.print("DFPlayer online, volume ");
  Serial.print(Sound::VOLUME_DEFAULT);
  Serial.print("/");
  Serial.println(Sound::VOLUME_MAX);
  Serial.println("Cycling all 5 tones. Each one repeats for 4 s, then 1.5 s of silence.");
}

void loop() {
  Sound::poll();

  if (!Sound::online()) return;

  if (!Sound::playing() && millis() >= nextStartMs) {
    Serial.print("track ");
    Serial.print(track + 1);
    Serial.print(" -> ");
    Serial.println(TONE_NAMES[track]);

    Sound::play(track, PLAY_SECONDS);
    track = (track + 1) % Sound::TRACK_COUNT;
    nextStartMs = millis() + (uint32_t)PLAY_SECONDS * 1000UL + GAP_MS;
  }
}
