#include <Arduino.h>
#include "config.h"
#include "wifi_setup.h"
#include "mega_link.h"
#include "web_control.h"
#include "arylic_metadata.h"

void setup() {
  Serial.begin(115200);
  megaLinkBegin();
  wifiSetupBegin();
  webControlBegin();
}

void loop() {
  wifiSetupMaintain();
  webControlPoll();
  pollArylicMetadata();
}
