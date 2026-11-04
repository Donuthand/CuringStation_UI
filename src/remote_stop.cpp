#include "remote_stop.h"

#include <stdio.h>
#include <string.h>

#include <WiFi.h>
#include <esp_mac.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "remote_stop_protocol.h"

namespace RemoteStop {

namespace {

using namespace RemoteStopProtocol;

void (*stopHandler)() = nullptr;
volatile bool pendingStop = false;
volatile uint32_t rawCount = 0;   // every packet heard, before filtering — diagnostic
volatile int lastRawLen = 0;

void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  (void)info;
  rawCount++;
  lastRawLen = len;
  if (len != (int)sizeof(Packet)) return;
  const Packet* p = reinterpret_cast<const Packet*>(data);
  if (memcmp(p->magic, MAGIC, sizeof(MAGIC)) != 0) return;
  if (p->version != VERSION) return;
  if (p->cmd != CMD_STOP_ALARM) return;
  pendingStop = true;   // ISR/task context — flag only, no TFT/Station calls here
}

} // namespace

bool begin(void (*onStop)()) {
  stopHandler = onStop;
  WiFi.mode(WIFI_STA);   // ESP-NOW needs the radio up; this does not join a network

  // WiFi.mode() starts the radio asynchronously. Reading the MAC or calling
  // esp_now_init() immediately after can race ahead of that — observed on hardware
  // 2026-09-14 as WiFi.macAddress() printing 00:00:00:00:00:00, which a real ESP32
  // never has. A short settle delay avoids relying on timing debt from calls made
  // earlier in setup() to have covered this by accident.
  delay(100);

  // Pin a fixed channel rather than trusting whatever the radio lands on by
  // default — button and station must agree on channel for ESP-NOW to work at
  // all, and nothing else here calls WiFi.begin() to pin one implicitly.
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[RemoteStop] esp_now_init failed - alarm stop button unavailable");
    return false;
  }

  esp_now_register_recv_cb(onDataRecv);

  // Read straight from eFuse rather than WiFi.macAddress(), which queries the
  // running WiFi driver and returned 00:00:00:00:00:00 on hardware 2026-09-14 —
  // this is what the driver being up depends on too, so it's a proxy for whether
  // ESP-NOW can actually receive, worth printing correctly rather than papering
  // over with a delay alone.
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.print("[RemoteStop] ready, station MAC ");
  Serial.print(macStr);
  Serial.print(", channel ");
  Serial.println(WiFi.channel());
  return true;
}

void poll() {
  static uint32_t printedRaw = 0;
  if (rawCount != printedRaw) {
    printedRaw = rawCount;
    Serial.printf("[RemoteStop] heard packet #%lu, len %d\n", (unsigned long)printedRaw, lastRawLen);
  }
  if (!pendingStop) return;
  pendingStop = false;
  Serial.println("[RemoteStop] STOP_ALARM received");
  if (stopHandler) stopHandler();
}

} // namespace RemoteStop
