#include <Arduino.h>
#include <ESPmDNS.h>
#include "config.h"
#include "wifi_setup.h"
#include "wifi_provisioning.h"
#include "mega_link.h"
#include "web_control.h"
#include "arylic_metadata.h"

// true, пока идёт настройка Wi-Fi (своя точка доступа, см. wifi_provisioning.h) — обычная
// работа (веб-управление/метадата) в этом режиме не имеет смысла, реальной сети ещё нет
static bool provisioning = false;

void setup() {
  Serial.begin(115200);
  megaLinkBegin();

  if (wifiSetupBegin()) {
    // Нужен для резолва ARYLIC_MDNS_HOSTNAME в arylic_metadata.cpp (MDNS.queryHost()) —
    // сама подсеть под этим именем ESP32 не анонсирует, только пользуется чужими анонсами.
    // Имя "esp32-audio-webctl" ни на что не влияет, обязателен сам факт вызова begin()
    MDNS.begin("esp32-audio-webctl");
    webControlBegin();
    arylicMetadataBegin();
  } else {
    // Сохранённой сети нет или она не отвечает (например устройство перенесли в другой дом) —
    // вместо обычной работы поднимаем AP-режим настройки, см. wifi_provisioning.h
    provisioning = true;
    wifiProvisioningBegin();
  }
}

void loop() {
  if (provisioning) {
    wifiProvisioningPoll();
    return;
  }
  wifiSetupMaintain();
  webControlPoll();
  megaLinkPoll();
}
