#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "config.h"
#include "wifi_setup.h"
#include "wifi_provisioning.h"
#include "mega_link.h"
#include "web_control.h"
#include "arylic_metadata.h"
#include "motor_ws.h"
#include "frame_mirror.h"
#include "weather.h"

// true, пока идёт настройка Wi-Fi (своя точка доступа, см. wifi_provisioning.h) — обычная
// работа (веб-управление/метадата) в этом режиме не имеет смысла, реальной сети ещё нет
static bool provisioning = false;

// Диагностика реального зависания (2026-10-20): пользователь подтвердил живым тестом на
// Меге (энкодер продолжает крутить моторы сам по себе), что виснет именно этот ESP32 целиком —
// ни веб-морда, ни WS к кнобу, ни UART к Меге не отвечают. Фикс TLS-хендшейка Arylic
// (setHandshakeTimeout) это НЕ исправил — значит, зависает где-то ещё в этом же loop().
//
// Вместо гадания дальше по коду — два прибора разом:
//  1. "Канарейка" — отдельная лёгкая FreeRTOS-задача, которая раз в секунду проверяет, сколько
//     времени текущий шаг loop() уже выполняется, и кричит в Serial СРАЗУ, как только это
//     подозрительно долго — НЕ дожидаясь, пока шаг наконец завершится (если он вообще
//     когда-нибудь завершится — настоящее зависание именно и означает, что не завершится).
//     Это единственный способ поймать истинный "висит вечно", не просто "долго, но живой".
//  2. Свободная куча раз в 10с — "работает немного, потом виснет" похоже на утечку/
//     фрагментацию кучи, эта метрика покажет её вживую, если дело в этом.
constexpr unsigned long kStuckWarnMs = 2000;
constexpr unsigned long kHeapLogIntervalMs = 10000;
unsigned long gLastHeapLogMs = 0;
volatile unsigned long gStepStartMs = 0;
const char *volatile gCurrentStep = "setup";
bool gStuckWarned = false;

void watchdogTaskFn(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    unsigned long stuckFor = millis() - gStepStartMs;
    if (stuckFor > kStuckWarnMs) {
      if (!gStuckWarned) {
        gStuckWarned = true;
        Serial.printf("[watchdog] STUCK in step '%s' for %lums and counting (uptime %lus)\n",
                      gCurrentStep, stuckFor, millis() / 1000);
      }
    } else {
      gStuckWarned = false;
    }
  }
}

template <typename Fn> void timedStep(const char *name, Fn fn) {
  gCurrentStep = name;
  gStepStartMs = millis();
  fn();
  unsigned long elapsed = millis() - gStepStartMs;
  if (elapsed >= 150) {
    Serial.printf("[loop] slow step: %s took %lums (uptime %lus)\n", name, elapsed,
                  millis() / 1000);
  }
}

void logHeapIfDue() {
  unsigned long now = millis();
  if (now - gLastHeapLogMs < kHeapLogIntervalMs) return;
  gLastHeapLogMs = now;
  Serial.printf("[heap] free=%u minFree=%u maxAlloc=%u uptime=%lus\n", ESP.getFreeHeap(),
                ESP.getMinFreeHeap(), ESP.getMaxAllocHeap(), now / 1000);
}

// Второй шаг заводского сброса (см. handleFactoryReset() в web_control.cpp за полным
// объяснением, почему сброс вообще разбит на два перезапуска — вкратце: гарантированно чистая,
// незафрагментированная куча для TLS-хендшейка к Arylic). Подключаемся ЕЩЁ валидными (ещё не
// стёртыми) сохранёнными кредами — минимально, только то, что нужно для mDNS + HTTPS до Arylic,
// без поднятия обычного стека сервисов (веб-сервер/WS/опрос) — незачем тратить на них только что
// освобождённую кучу прямо перед тем, как всё равно всё стереть и перезагрузиться ещё раз.
static void runPendingFactoryReset() {
  Serial.println("[reset] второй шаг заводского сброса — подключаюсь ещё валидными кредами");
  String ssid = wifiSavedSsid();
  String password = wifiSavedPassword();
  if (ssid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    unsigned long start = millis();
    while (millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
      if (WiFi.status() == WL_CONNECTED) break;
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("[reset] подключился — кидаю заводской сброс на Arylic со свежей кучей");
      MDNS.begin("esp32-audio-webctl");
      arylicMetadataInitMutex(); // resolveArylicIp() нужен мьютекс, сам опрос не поднимаем
      arylicTriggerFactoryReset();
    } else {
      Serial.println("[reset] не подключился к сохранённой сети — Arylic сбросить не смог, пропускаю");
    }
  } else {
    Serial.println("[reset] сохранённой сети не было — Arylic сбросить не смог, пропускаю");
  }
  Serial.println("[reset] стираю Wi-Fi и ухожу в режим настройки");
  wifiForgetCredentials();
  delay(1000); // тот же запас, что был у прежней однопроходной версии — перед тем как рвать сеть
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  gStepStartMs = millis();
  xTaskCreate(watchdogTaskFn, "hangwatch", 2048, nullptr, 1, nullptr);
  megaLinkBegin();

  if (wifiConsumeFactoryResetPending()) {
    runPendingFactoryReset(); // не возвращается — всегда заканчивается ESP.restart()
    return;
  }

  if (wifiSetupBegin()) {
    // Нужен для резолва ARYLIC_MDNS_HOSTNAME в arylic_metadata.cpp (MDNS.queryHost()) —
    // сама подсеть под этим именем ESP32 не анонсирует, только пользуется чужими анонсами.
    // Имя "esp32-audio-webctl" ни на что не влияет, обязателен сам факт вызова begin()
    MDNS.begin("esp32-audio-webctl");
    webControlBegin();
    arylicMetadataBegin();
    weatherBegin();
    motorWsBegin();
    frameMirrorBegin();
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
  timedStep("wifiSetupMaintain", wifiSetupMaintain);
  timedStep("webControlPoll", webControlPoll);
  timedStep("megaLinkPoll", megaLinkPoll);
  timedStep("motorWsPoll", motorWsPoll);
  timedStep("frameMirrorPoll", frameMirrorPoll);
  logHeapIfDue();
}
