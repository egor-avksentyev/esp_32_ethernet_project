#include "mega_link.h"
#include "config.h"
#include "web_control.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>
#include <stdlib.h>

static HardwareSerial MegaSerial(2);

// 64, не 32 — SCR:<name>:<inSettings>:<line1>:<line2>:<highlight> может доходить до полусотни
// символов при длинных line1/line2 (например Dimmer: "SCR:Dimmer:1:LED 100%:Display 100%:2")
static char lineBuf[64];
static uint8_t lineLen = 0;

static bool megaPowerKnown = false;
static bool megaPoweredOn = false;
static float megaTemps[3] = {-127.0f, -127.0f, -127.0f}; // TEMP_SENSOR_INVALID на стороне Mega
static bool megaVoltageKnown = false;
static int megaVoltage = 0;

static bool megaScreenKnown = false;
static char megaScreenName[16] = "";
static bool megaScreenInSettings = false;
static char megaScreenLine1[20] = "";
static char megaScreenLine2[20] = "";
static uint8_t megaScreenHighlight = 0;
static bool megaScreenIsColorFlag = false;
static uint8_t megaScreenColorR = 0, megaScreenColorG = 0, megaScreenColorB = 0;

static bool megaMuteKnownFlag = false;
static bool megaMutedFlag = false;
static bool megaBypassKnownFlag = false;
static bool megaBypassOnFlag = false;
static bool megaStreamerKnownFlag = false;
static bool megaStreamerOnFlag = false;

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
  } else if (strncmp(line, "SCR:", 4) == 0) {
    char* p = line + 4;
    char* sep1 = strchr(p, ':'); if (!sep1) return;
    *sep1 = '\0'; char* name = p; p = sep1 + 1;
    char* sep2 = strchr(p, ':'); if (!sep2) return;
    *sep2 = '\0'; char* inSet = p; p = sep2 + 1;
    char* sep3 = strchr(p, ':'); if (!sep3) return;
    *sep3 = '\0'; char* l1 = p; p = sep3 + 1;
    char* sep4 = strchr(p, ':'); if (!sep4) return;
    *sep4 = '\0'; char* l2 = p; char* hl = sep4 + 1;
    megaScreenKnown = true;
    megaScreenIsColorFlag = false;
    strncpy(megaScreenName, name, sizeof(megaScreenName) - 1);
    megaScreenName[sizeof(megaScreenName) - 1] = '\0';
    megaScreenInSettings = (inSet[0] == '1');
    strncpy(megaScreenLine1, l1, sizeof(megaScreenLine1) - 1);
    megaScreenLine1[sizeof(megaScreenLine1) - 1] = '\0';
    strncpy(megaScreenLine2, l2, sizeof(megaScreenLine2) - 1);
    megaScreenLine2[sizeof(megaScreenLine2) - 1] = '\0';
    megaScreenHighlight = (uint8_t)atoi(hl);
  } else if (strncmp(line, "COLOR:", 6) == 0) {
    char* p = line + 6;
    char* sep1 = strchr(p, ':'); if (!sep1) return;
    *sep1 = '\0'; char* name = p; p = sep1 + 1;
    char* sep2 = strchr(p, ':'); if (!sep2) return;
    *sep2 = '\0'; char* rStr = p; p = sep2 + 1;
    char* sep3 = strchr(p, ':'); if (!sep3) return;
    *sep3 = '\0'; char* gStr = p; char* bStr = sep3 + 1;
    megaScreenKnown = true;
    megaScreenIsColorFlag = true;
    megaScreenInSettings = true;
    strncpy(megaScreenName, "Color", sizeof(megaScreenName) - 1);
    megaScreenName[sizeof(megaScreenName) - 1] = '\0';
    strncpy(megaScreenLine1, name, sizeof(megaScreenLine1) - 1);
    megaScreenLine1[sizeof(megaScreenLine1) - 1] = '\0';
    megaScreenLine2[0] = '\0';
    megaScreenHighlight = 0;
    megaScreenColorR = (uint8_t)atoi(rStr);
    megaScreenColorG = (uint8_t)atoi(gStr);
    megaScreenColorB = (uint8_t)atoi(bStr);
  } else if (strncmp(line, "MUTE:", 5) == 0 && (line[5] == '0' || line[5] == '1') && line[6] == '\0') {
    megaMuteKnownFlag = true;
    megaMutedFlag = (line[5] == '1');
  } else if (strncmp(line, "BYP:", 4) == 0 && (line[4] == '0' || line[4] == '1') && line[5] == '\0') {
    megaBypassKnownFlag = true;
    megaBypassOnFlag = (line[4] == '1');
  } else if (strncmp(line, "STREAMER:", 9) == 0 && (line[9] == '0' || line[9] == '1') && line[10] == '\0') {
    megaStreamerKnownFlag = true;
    megaStreamerOnFlag = (line[9] == '1');
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

bool megaLinkScreenKnown() {
  return megaScreenKnown;
}

const char* megaLinkScreenName() {
  return megaScreenName;
}

bool megaLinkScreenInSettings() {
  return megaScreenInSettings;
}

const char* megaLinkScreenLine1() {
  return megaScreenLine1;
}

const char* megaLinkScreenLine2() {
  return megaScreenLine2;
}

uint8_t megaLinkScreenHighlight() {
  return megaScreenHighlight;
}

bool megaLinkScreenIsColor() {
  return megaScreenIsColorFlag;
}

uint8_t megaLinkScreenColorR() {
  return megaScreenColorR;
}

uint8_t megaLinkScreenColorG() {
  return megaScreenColorG;
}

uint8_t megaLinkScreenColorB() {
  return megaScreenColorB;
}

bool megaLinkMuteKnown() {
  return megaMuteKnownFlag;
}

bool megaLinkIsMuted() {
  return megaMutedFlag;
}

bool megaLinkBypassKnown() {
  return megaBypassKnownFlag;
}

bool megaLinkIsBypassOn() {
  return megaBypassOnFlag;
}

bool megaLinkStreamerKnown() {
  return megaStreamerKnownFlag;
}

bool megaLinkIsStreamerOn() {
  return megaStreamerOnFlag;
}
