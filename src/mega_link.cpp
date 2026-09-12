#include "mega_link.h"
#include "config.h"

static HardwareSerial MegaSerial(2);

void megaLinkBegin() {
  MegaSerial.begin(MEGA_LINK_BAUD, SERIAL_8N1, MEGA_LINK_RX_PIN, MEGA_LINK_TX_PIN);
}

void megaLinkSendCommand(char actionLetter) {
  MegaSerial.print("CMD:");
  MegaSerial.print(actionLetter);
  MegaSerial.print('\n');
}

void megaLinkSendMetadata(const char* text) {
  MegaSerial.print("META:");
  size_t len = strlen(text);
  if (len > MEGA_LINK_META_MAX_LEN) {
    len = MEGA_LINK_META_MAX_LEN;
  }
  MegaSerial.write((const uint8_t*)text, len);
  MegaSerial.print('\n');
}

void megaLinkSendIp(const IPAddress& ip) {
  MegaSerial.print("IP:");
  MegaSerial.print(ip);
  MegaSerial.print('\n');
}

void megaLinkSendPlayState(bool playing) {
  MegaSerial.print("PLAY:");
  MegaSerial.print(playing ? '1' : '0');
  MegaSerial.print('\n');
}

void megaLinkSendArylicStatus(bool ok) {
  MegaSerial.print("ARYLIC:");
  MegaSerial.print(ok ? "OK" : "FAIL");
  MegaSerial.print('\n');
}

void megaLinkSendSource(const char* name) {
  MegaSerial.print("SRC:");
  MegaSerial.print(name); // короткое (см. computeSourceName), MEGA_LINK_META_MAX_LEN не нужен
  MegaSerial.print('\n');
}
