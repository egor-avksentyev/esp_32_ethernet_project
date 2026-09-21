#include "mega_link.h"
#include "config.h"
#include "web_control.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>
#include <stdlib.h>

static HardwareSerial MegaSerial(2);

static char lineBuf[32];
static uint8_t lineLen = 0;

static bool megaPowerKnown = false;
static bool megaPoweredOn = false;
static float megaTemps[3] = {-127.0f, -127.0f, -127.0f}; // TEMP_SENSOR_INVALID на стороне Mega
static bool megaVoltageKnown = false;
static int megaVoltage = 0;

// Построчный приём — тот же приём, что esp32LinkPoll() на стороне Mega (esp32_link.cpp):
// копим байты в статический буфер, разбираем по '\n', отбрасываем '\r'
static void handleMegaLine(char* line) {
  if (strncmp(line, "POWER:", 6) == 0 && (line[6] == '0' || line[6] == '1') && line[7] == '\0') {
    megaPowerKnown = true;
    megaPoweredOn = (line[6] == '1');
    // Реальное состояние с пульта/энкодера самой Mega — та же точка входа, что у кнопки
    // Power на этой веб-странице (см. applyWebPowerState() в web_control.cpp)
    applyWebPowerState(!megaPoweredOn);
  } else if (strncmp(line, "TEMP:", 5) == 0) {
    char* p = line + 5;
    for (uint8_t i = 0; i < 3; i++) {
      char* sep = (i < 2) ? strchr(p, ':') : nullptr;
      if (i < 2 && !sep) {
        return; // битая строка — не полагаемся на частично разобранные значения
      }
      if (sep) {
        *sep = '\0';
      }
      megaTemps[i] = atof(p);
      if (sep) {
        p = sep + 1;
      }
    }
  } else if (strncmp(line, "VOLT:", 5) == 0) {
    megaVoltageKnown = true;
    megaVoltage = atoi(line + 5);
  }
}

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

// Приём не требует MegaMutexGuard — тот защищает только запись (несколько задач шлют
// одновременно, см. комментарий у megaMutex выше), читает всегда один и тот же вызывающий
// (loop() на первом ядре), гонки с этой стороны нет
void megaLinkPoll() {
  while (MegaSerial.available()) {
    char c = MegaSerial.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        handleMegaLine(lineBuf);
      }
      lineLen = 0;
      continue;
    }
    if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}

bool megaLinkPowerKnown() {
  return megaPowerKnown;
}

bool megaLinkIsPoweredOn() {
  return megaPoweredOn;
}

float megaLinkTemp(uint8_t index) {
  if (index >= 3) {
    return -127.0f;
  }
  return megaTemps[index];
}

bool megaLinkVoltageKnown() {
  return megaVoltageKnown;
}

int megaLinkVoltage() {
  return megaVoltage;
}
