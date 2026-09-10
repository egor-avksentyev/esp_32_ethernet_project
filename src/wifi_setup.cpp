#include "wifi_setup.h"
#include "config.h"
#include "wifi_credentials.h"
#include <WiFi.h>

void wifiSetupBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[wifi] connecting to ");
  Serial.println(WIFI_SSID);
}

void wifiSetupMaintain() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < WIFI_RECONNECT_CHECK_MS) {
    return;
  }
  lastCheck = millis();

  if (WiFi.status() == WL_CONNECTED) {
    return;
  }
  // WiFi.begin() тут не блокирует надолго — сам факт вызова инициирует (пере)подключение,
  // статус проверяется на следующих тиках, не в этом же вызове
  Serial.println("[wifi] not connected, retrying...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

bool wifiIsConnected() {
  return WiFi.status() == WL_CONNECTED;
}
