#include "arylic_metadata.h"
#include "config.h"
#include "wifi_setup.h"
#include <WiFi.h>
#include <HTTPClient.h>

static String arylicUrl() {
  IPAddress ip(ARYLIC_IP_OCTETS);
  String url = "http://";
  url += ip.toString();
  url += "/httpapi.asp?command=getPlayerStatus";
  return url;
}

void pollArylicMetadata() {
  static unsigned long lastPoll = 0;
  if (!wifiIsConnected() || millis() - lastPoll < ARYLIC_POLL_INTERVAL_MS) {
    return;
  }
  lastPoll = millis();

  HTTPClient http;
  http.setTimeout(2000); // локальная сеть — секунды с запасом на "не отвечает/выключен"
  http.begin(arylicUrl());
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.print("[arylic] запрос не удался, код: ");
    Serial.println(httpCode);
    http.end();
    return;
  }

  // Диагностика: печатаем ВЕСЬ сырой ответ в Serial, чтобы увидеть реальный формат перед
  // тем, как писать парсинг — см. комментарий в arylic_metadata.h. Как только формат будет
  // подтверждён на реальном железе, здесь появится разбор title/artist и вызов
  // megaLinkSendMetadata() с готовой строкой
  String payload = http.getString();
  Serial.println("[arylic] raw response:");
  Serial.println(payload);

  http.end();
}
