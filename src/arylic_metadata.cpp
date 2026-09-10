#include "arylic_metadata.h"
#include "config.h"
#include "wifi_setup.h"
#include "mega_link.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

static String arylicUrl() {
  IPAddress ip(ARYLIC_IP_OCTETS);
  String url = "https://";
  url += ip.toString();
  url += "/httpapi.asp?command=getPlayerStatus";
  return url;
}

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// "4A757374" -> "Just" — Title/Artist/Album у Arylic закодированы парами hex-цифр
// (см. arylic_metadata.h). Останавливается на первой некорректной паре — этого достаточно,
// чтобы не упасть на мусоре, полноценная валидация тут не нужна (источник — локальное
// доверенное устройство, не пользовательский ввод)
static void decodeHexField(const char* hex, char* out, size_t outMax) {
  size_t o = 0;
  for (size_t i = 0; hex[i] && hex[i + 1] && o < outMax - 1; i += 2) {
    int hi = hexNibble(hex[i]);
    int lo = hexNibble(hex[i + 1]);
    if (hi < 0 || lo < 0) break;
    out[o++] = (char)((hi << 4) | lo);
  }
  out[o] = '\0';
}

// Простой strstr по плоскому JSON — ответ Arylic одноуровневый (см. arylic_metadata.h),
// полноценный JSON-парсер тут не нужен и не стоит своего RAM
static void extractHexField(const String& payload, const char* key, char* out, size_t outMax) {
  out[0] = '\0';
  int idx = payload.indexOf(key);
  if (idx < 0) {
    return;
  }
  const char* start = payload.c_str() + idx + strlen(key);
  const char* end = strchr(start, '"');
  if (!end) {
    return;
  }
  char hexBuf[80];
  size_t len = end - start;
  if (len >= sizeof(hexBuf)) {
    len = sizeof(hexBuf) - 1;
  }
  memcpy(hexBuf, start, len);
  hexBuf[len] = '\0';
  decodeHexField(hexBuf, out, outMax);
}

void pollArylicMetadata() {
  static unsigned long lastPoll = 0;
  if (!wifiIsConnected() || millis() - lastPoll < ARYLIC_POLL_INTERVAL_MS) {
    return;
  }
  lastPoll = millis();

  WiFiClientSecure client;
  client.setInsecure(); // самоподписанный сертификат Arylic — цепочку не проверяем

  HTTPClient http;
  http.setTimeout(2000); // локальная сеть — секунды с запасом на "не отвечает/выключен"
  http.begin(client, arylicUrl());
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.print("[arylic] запрос не удался, код: ");
    Serial.println(httpCode);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  if (payload.indexOf("\"status\":\"play\"") < 0) {
    // Не играет (pause/stop/idle) — метадату не шлём. Что показывать на Mega в этом случае —
    // открытый вопрос, см. README.md, решается на стороне Mega-приёмника, не здесь
    return;
  }

  char title[32];
  char artist[32];
  extractHexField(payload, "\"Title\":\"", title, sizeof(title));
  extractHexField(payload, "\"Artist\":\"", artist, sizeof(artist));

  if (title[0] == '\0' && artist[0] == '\0') {
    Serial.println("[arylic] играет, но Title/Artist не найдены в ответе — сырой ответ:");
    Serial.println(payload);
    return;
  }

  char combined[MEGA_LINK_META_MAX_LEN + 1];
  if (artist[0] && title[0]) {
    snprintf(combined, sizeof(combined), "%s - %s", artist, title);
  } else {
    snprintf(combined, sizeof(combined), "%s%s", artist, title);
  }

  megaLinkSendMetadata(combined);
  Serial.print("[arylic] ");
  Serial.println(combined);
}
