#include "wifi_setup.h"
#include "config.h"
#include <WiFi.h>
#include <Preferences.h>

static volatile bool connected = false;
static unsigned long currentBackoffMs = WIFI_RECONNECT_BASE_MS;
static unsigned long nextReconnectAttempt = 0;

static void onWifiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      connected = true;
      currentBackoffMs = WIFI_RECONNECT_BASE_MS; // сброс паузы после успешного подключения
      Serial.print("[wifi] подключено, IP: ");
      Serial.println(WiFi.localIP());
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
  WiFi.mode(WIFI_STA);
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

void wifiSaveCredentials(const char* ssid, const char* password) {
  Preferences prefs;
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", password);
  prefs.end();
}

void wifiForgetCredentials() {
  Preferences prefs;
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
}
