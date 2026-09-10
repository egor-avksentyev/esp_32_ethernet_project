#include <Arduino.h>
#include <ESPmDNS.h>
#include "config.h"
#include "wifi_setup.h"
#include "mega_link.h"
#include "web_control.h"
#include "arylic_metadata.h"

void setup() {
  Serial.begin(115200);
  megaLinkBegin();
  wifiSetupBegin();
  // Нужен для резолва ARYLIC_MDNS_HOSTNAME в arylic_metadata.cpp (MDNS.queryHost()) —
  // сама подсеть под этим именем ESP32 не анонсирует, только пользуется чужими анонсами.
  // Имя "esp32-audio-webctl" ни на что не влияет, обязателен сам факт вызова begin()
  MDNS.begin("esp32-audio-webctl");
  webControlBegin();
}

void loop() {
  wifiSetupMaintain();
  webControlPoll();
  pollArylicMetadata();
}
