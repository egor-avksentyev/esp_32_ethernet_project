#include "wifi_setup.h"
#include "config.h"
#include "mega_link.h"
#include <WiFi.h>
#include <Preferences.h>

static volatile bool connected = false;
static unsigned long currentBackoffMs = WIFI_RECONNECT_BASE_MS;
static unsigned long nextReconnectAttempt = 0;
static unsigned long lastIpSend = 0;

static void onWifiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      connected = true;
      currentBackoffMs = WIFI_RECONNECT_BASE_MS; // сброс паузы после успешного подключения
      Serial.print("[wifi] подключено, IP: ");
      Serial.println(WiFi.localIP());
      megaLinkSendIp(WiFi.localIP());
      lastIpSend = millis();
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      if (connected) {
        Serial.println("[wifi] связь потеряна — переподключение с нарастающей паузой");
      }
      connected = false;
      nextReconnectAttempt = millis() + currentBackoffMs;
      break;
    default:
      break;
  }
}

bool wifiSetupBegin() {
  Preferences prefs;
  prefs.begin("wifi", true); // read-only
  String ssid = prefs.getString("ssid", "");
  String password = prefs.getString("pass", "");
  prefs.end();

  if (ssid.length() == 0) {
    Serial.println("[wifi] сохранённой сети нет — нужна настройка (см. wifi_provisioning.h)");
    return false;
  }

  WiFi.onEvent(onWifiEvent);
  // AP_STA, не чистый STA — ARCHITECTURE.md "Wi-Fi handoff": держим домашнюю сеть И служебную
  // точку для кнопа одновременно (единственное ограничение — пока САМ ESP32 ещё не настроен и
  // показывает WIFI_PROVISION_AP_SSID вместо этой, см. wifi_provisioning.cpp, радио тогда занято)
  WiFi.mode(WIFI_AP_STA);
  // Найдено живьём (2026-10-21): пользователь пожаловался на редкие запинки даже после фикса
  // зеркала; `ping` до этой платы с Мака показал задержку, скачущую 18-100мс на пустом месте
  // (должно быть пара мс в локальной сети) — классическая подпись Wi-Fi modem-sleep
  // (WIFI_PS_MIN_MODEM, включён по умолчанию): рация дремлет между битами маячка роутера,
  // просыпаясь по требованию. Это было отключено на кнобе (wifi_driver.cpp) ещё раньше, но
  // никогда не было отключено ЗДЕСЬ, на компаньоне — тот же приём, симметрично
  WiFi.setSleep(false);
  WiFi.softAP(WIFI_SERVICE_AP_SSID, WIFI_SERVICE_AP_PASSWORD);
  Serial.print("[wifi] служебная точка для кнопа поднята: ");
  Serial.println(WIFI_SERVICE_AP_SSID);
  WiFi.begin(ssid.c_str(), password.c_str());
  Serial.print("[wifi] connecting to ");
  Serial.println(ssid);

  // Одноразовое ожидание в setup() (не в loop()) — тот же принцип, что уже применялся в
  // Mega-проекте для Ethernet.begin()/DHCP: разовая стоимость при старте, приемлемо
  unsigned long start = millis();
  while (millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      nextReconnectAttempt = millis() + currentBackoffMs;
      return true;
    }
    delay(100);
  }

  Serial.println("[wifi] сохранённая сеть не отвечает — вероятно устройство в новом месте");
  return false;
}

void wifiSetupMaintain() {
  if (connected) {
    // Периодически повторяем IP на Mega — не только в момент подключения (см. config.h,
    // WIFI_IP_RESEND_INTERVAL_MS, за тем зачем: Mega могла пропустить самое первое сообщение)
    if (millis() - lastIpSend >= WIFI_IP_RESEND_INTERVAL_MS) {
      lastIpSend = millis();
      megaLinkSendIp(WiFi.localIP());
    }
    return;
  }
  // Безопасно к переполнению millis() (знаковая разность) — актуально, раз устройство может
  // работать неделями без перезагрузки
  if ((long)(millis() - nextReconnectAttempt) < 0) {
    return; // ещё не время для следующей попытки
  }

  Serial.print("[wifi] попытка переподключения, следующая пауза: ");
  Serial.print(currentBackoffMs * 2 > WIFI_RECONNECT_MAX_MS ? WIFI_RECONNECT_MAX_MS : currentBackoffMs * 2);
  Serial.println("мс");
  WiFi.reconnect(); // переиспользует SSID/пароль из wifiSetupBegin(), заново их передавать не нужно

  currentBackoffMs = (currentBackoffMs * 2 > WIFI_RECONNECT_MAX_MS) ? WIFI_RECONNECT_MAX_MS : currentBackoffMs * 2;
  nextReconnectAttempt = millis() + currentBackoffMs;
}

bool wifiIsConnected() {
  return connected;
}

// Общий инкремент — вызывается из обеих функций ниже, одна точка правды вместо дублирования
// "+1 и записать обратно" в двух местах
static void bumpCredGen(Preferences& prefs) {
  unsigned long gen = prefs.getULong("gen", 0);
  prefs.putULong("gen", gen + 1);
}

void wifiSaveCredentials(const char* ssid, const char* password) {
  Preferences prefs;
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", password);
  bumpCredGen(prefs);
  prefs.end();
}

void wifiForgetCredentials() {
  Preferences prefs;
  prefs.begin("wifi", false);
  // Найдено живьём: prefs.clear() стёр бы и сам "gen" тоже — следующий wifiSaveCredentials()
  // после него снова начал бы с 0, что кноп (уже видевший более высокий gen раньше) расценил
  // бы как "откат назад", не как новое поколение — bumpCredGen() должен отработать ПОСЛЕ clear,
  // чтобы итоговое значение гарантированно оказалось строго больше всего, что было раньше
  prefs.clear();
  bumpCredGen(prefs);
  prefs.end();
}

unsigned long wifiCredGen() {
  Preferences prefs;
  prefs.begin("wifi", true);
  unsigned long gen = prefs.getULong("gen", 0);
  prefs.end();
  return gen;
}

String wifiSavedSsid() {
  Preferences prefs;
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  prefs.end();
  return ssid;
}

String wifiSavedPassword() {
  Preferences prefs;
  prefs.begin("wifi", true);
  String password = prefs.getString("pass", "");
  prefs.end();
  return password;
}

void wifiMarkFactoryResetPending() {
  Preferences prefs;
  prefs.begin("wifi", false);
  prefs.putBool("resetPending", true);
  prefs.end();
}

bool wifiConsumeFactoryResetPending() {
  Preferences prefs;
  prefs.begin("wifi", false);
  bool pending = prefs.getBool("resetPending", false);
  if (pending) {
    prefs.putBool("resetPending", false);
  }
  prefs.end();
  return pending;
}
