#include "wifi_setup.h"
#include "config.h"
#include "wifi_credentials.h"
#include <WiFi.h>

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
      // Следующая попытка не сразу — WiFi.reconnect() дёргаем из wifiSetupMaintain() не
      // раньше, чем истечёт currentBackoffMs с этого момента (см. там же)
      nextReconnectAttempt = millis() + currentBackoffMs;
      break;
    default:
      break;
  }
}

void wifiSetupBegin() {
  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[wifi] connecting to ");
  Serial.println(WIFI_SSID);
  nextReconnectAttempt = millis() + currentBackoffMs;
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
