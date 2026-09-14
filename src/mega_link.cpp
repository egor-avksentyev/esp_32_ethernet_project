#include "mega_link.h"
#include "config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static HardwareSerial MegaSerial(2);

// Каждая send*() ниже — это несколько ОТДЕЛЬНЫХ MegaSerial.print(), которые вместе должны
// попасть на UART одной строкой. Раньше единственным вызывающим был основной loop() (веб-сервер
// + периодическая отправка IP), но теперь опрос Arylic (arylic_metadata.cpp) крутится в своей
// задаче на втором ядре и тоже шлёт сюда — без мьютекса куски двух вызовов могли бы перемежаться
// и Mega получила бы битую строку. Критическая секция короткая — просто несколько print()
static SemaphoreHandle_t megaMutex = nullptr;

struct MegaMutexGuard {
  MegaMutexGuard() { xSemaphoreTake(megaMutex, portMAX_DELAY); }
  ~MegaMutexGuard() { xSemaphoreGive(megaMutex); }
};

void megaLinkBegin() {
  megaMutex = xSemaphoreCreateMutex();
  MegaSerial.begin(MEGA_LINK_BAUD, SERIAL_8N1, MEGA_LINK_RX_PIN, MEGA_LINK_TX_PIN);
}

void megaLinkSendCommand(char actionLetter) {
  MegaMutexGuard g;
  MegaSerial.print("CMD:");
  MegaSerial.print(actionLetter);
  MegaSerial.print('\n');
}

void megaLinkSendMetadata(const char* text) {
  MegaMutexGuard g;
  MegaSerial.print("META:");
  size_t len = strlen(text);
  if (len > MEGA_LINK_META_MAX_LEN) {
    len = MEGA_LINK_META_MAX_LEN;
  }
  MegaSerial.write((const uint8_t*)text, len);
  MegaSerial.print('\n');
}

void megaLinkSendIp(const IPAddress& ip) {
  MegaMutexGuard g;
  MegaSerial.print("IP:");
  MegaSerial.print(ip);
  MegaSerial.print('\n');
}

void megaLinkSendPlayState(bool playing) {
  MegaMutexGuard g;
  MegaSerial.print("PLAY:");
  MegaSerial.print(playing ? '1' : '0');
  MegaSerial.print('\n');
}

void megaLinkSendArylicStatus(bool ok) {
  MegaMutexGuard g;
  MegaSerial.print("ARYLIC:");
  MegaSerial.print(ok ? "OK" : "FAIL");
  MegaSerial.print('\n');
}

void megaLinkSendSource(const char* name) {
  MegaMutexGuard g;
  MegaSerial.print("SRC:");
  MegaSerial.print(name); // короткое (см. computeSourceName), MEGA_LINK_META_MAX_LEN не нужен
  MegaSerial.print('\n');
}

void megaLinkSendPosition(long posMs, long lenMs) {
  MegaMutexGuard g;
  MegaSerial.print("POS:");
  MegaSerial.print(posMs);
  MegaSerial.print(':');
  MegaSerial.print(lenMs);
  MegaSerial.print('\n');
}
