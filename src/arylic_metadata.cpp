#include "arylic_metadata.h"
#include "config.h"
#include "wifi_setup.h"
#include "mega_link.h"
#include <ctype.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>

// Резолвит ARYLIC_MDNS_HOSTNAME через mDNS (см. config.h — имя найдено live через
// `dns-sd -B _linkplay._tcp local.`, привязано к устройству через его MAC, не к текущему
// IP). Кэшируется, пока запросы к Arylic проходят успешно — перерезолвливается заново
// только если pollArylicMetadata() получит отказ (см. ниже invalidateArylicIp()), а не
// на каждый опрос: сам mDNS-запрос — блокирующий (см. MDNS.queryHost()), незачем платить
// эту цену каждые ARYLIC_POLL_INTERVAL_MS, когда IP скорее всего не менялся
static IPAddress arylicIp;
static bool arylicIpKnown = false;
static bool reachable = false;

// Ручной ввод с веб-страницы (setArylicIpOverride()) — пока задан, имеет приоритет над
// mDNS/статическим фолбэком целиком, resolveArylicIp() до них даже не доходит
static IPAddress manualIp;
static bool manualIpSet = false;

// Прогресс трека для веб-страницы (см. arylic_metadata.h) — отдельная копия от того, что
// уходит на Mega (там своя, короче, MEGA_LINK_META_MAX_LEN)
static char trackText[64] = "";
static long trackPosMs = 0;
static long trackLenMs = 0;
static unsigned long trackCaptureMillis = 0;
static bool trackPlaying = false;
static char trackArtUrl[160] = "";
static char trackSourceName[24] = "";

bool arylicTrackIsPlaying() { return trackPlaying; }
String arylicTrackText() { return String(trackText); }
long arylicTrackPosMs() { return trackPosMs; }
long arylicTrackLenMs() { return trackLenMs; }
unsigned long arylicTrackAgeMs() { return millis() - trackCaptureMillis; }
String arylicTrackArtUrl() { return String(trackArtUrl); }
String arylicTrackSourceName() { return String(trackSourceName); }

static void invalidateArylicIp() {
  arylicIpKnown = false;
}

bool arylicIsReachable() {
  return reachable;
}

String arylicCurrentIp() {
  if (manualIpSet) {
    return manualIp.toString();
  }
  if (arylicIpKnown) {
    return arylicIp.toString();
  }
  return "";
}

void setArylicIpOverride(const char* ip) {
  if (ip[0] == '\0') {
    manualIpSet = false;
    invalidateArylicIp(); // на следующем опросе снова резолвим через mDNS с чистого листа
    Serial.println("[arylic] ручной IP снят, возвращаюсь к mDNS");
    return;
  }
  IPAddress parsed;
  if (!parsed.fromString(ip)) {
    Serial.print("[arylic] некорректный IP с веб-страницы: ");
    Serial.println(ip);
    return;
  }
  manualIp = parsed;
  manualIpSet = true;
  Serial.print("[arylic] IP задан вручную: ");
  Serial.println(manualIp);
}

static IPAddress resolveArylicIp() {
  if (manualIpSet) {
    return manualIp;
  }
  if (arylicIpKnown) {
    return arylicIp;
  }
  IPAddress resolved = MDNS.queryHost(ARYLIC_MDNS_HOSTNAME, 2000);
  if (resolved != IPAddress((uint32_t)0)) {
    arylicIp = resolved;
    arylicIpKnown = true;
    Serial.print("[arylic] mDNS: ");
    Serial.print(ARYLIC_MDNS_HOSTNAME);
    Serial.print(".local -> ");
    Serial.println(arylicIp);
  } else {
    arylicIp = IPAddress(ARYLIC_IP_OCTETS);
    arylicIpKnown = true; // не долбим mDNS каждый опрос и на фолбэке — тоже до следующего сбоя
    Serial.print("[arylic] mDNS-резолв не удался, использую статический IP из config.h: ");
    Serial.println(arylicIp);
  }
  return arylicIp;
}

static String arylicUrl() {
  String url = "https://";
  url += resolveArylicIp().toString();
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

// "curpos"/"totlen" у Arylic — простые строки-числа (не hex, в отличие от Title/Artist),
// поэтому проще: найти ключ и разобрать atol() — она сама останавливается на первом
// нечисловом символе (закрывающая кавычка), явно искать конец строки не нужно
static long extractLongField(const String& payload, const char* key) {
  int idx = payload.indexOf(key);
  if (idx < 0) {
    return 0;
  }
  return atol(payload.c_str() + idx + strlen(key));
}

// "mode" — тоже простая строка (не hex), но не число — код источника ("1" = AirPlay,
// "31" = Spotify Connect, проверено live). Та же логика поиска закрывающей кавычки, что у
// extractHexField, просто без hex-декодирования
static void extractStringField(const String& payload, const char* key, char* out, size_t outMax) {
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
  size_t len = end - start;
  if (len >= outMax) {
    len = outMax - 1;
  }
  memcpy(out, start, len);
  out[len] = '\0';
}

// Источник воспроизведения — сначала пробуем "vendor" (у Spotify Connect это
// "spotify:playlist:..."/"spotify:track:..." — берём схему до первого ":"), а если он
// пуст — откатываемся на "mode" (у AirPlay vendor всегда пуст, см. project_arylic_
// airplay_no_metadata в памяти Mega-репозитория). Официальная таблица кодов mode
// (github.com/AndersFluur/LinkPlayApi) не совпадает с тем, что реально отдаёт это
// устройство (там нет кода 31, который мы видим live для Spotify Connect) — поэтому здесь
// только те коды, что подтверждены живьём или взяты из документации как стабильные общие
// случаи, никаких догадок
static void computeSourceName(const String& payload) {
  char vendor[48];
  extractStringField(payload, "\"vendor\":\"", vendor, sizeof(vendor));
  if (vendor[0] != '\0') {
    char* colon = strchr(vendor, ':');
    if (colon) {
      *colon = '\0';
    }
    if (strcasecmp(vendor, "spotify") == 0) {
      strcpy(trackSourceName, "Spotify");
    } else if (strcasecmp(vendor, "tidal") == 0) {
      strcpy(trackSourceName, "Tidal");
    } else if (strcasecmp(vendor, "deezer") == 0) {
      strcpy(trackSourceName, "Deezer");
    } else if (strcasecmp(vendor, "qobuz") == 0) {
      strcpy(trackSourceName, "Qobuz");
    } else if (strcasecmp(vendor, "amazon") == 0) {
      strcpy(trackSourceName, "Amazon Music");
    } else {
      snprintf(trackSourceName, sizeof(trackSourceName), "%s", vendor);
      trackSourceName[0] = toupper(trackSourceName[0]);
    }
    return;
  }

  char mode[8];
  extractStringField(payload, "\"mode\":\"", mode, sizeof(mode));
  if (strcmp(mode, "1") == 0) {
    strcpy(trackSourceName, "AirPlay"); // подтверждено live 2026-09-12
  } else if (strcmp(mode, "2") == 0) {
    strcpy(trackSourceName, "DLNA");
  } else if (strcmp(mode, "40") == 0) {
    strcpy(trackSourceName, "Line-In");
  } else if (strcmp(mode, "41") == 0) {
    strcpy(trackSourceName, "Bluetooth");
  } else if (strcmp(mode, "43") == 0) {
    strcpy(trackSourceName, "Optical");
  } else {
    trackSourceName[0] = '\0'; // неизвестный код — не гадаем, лучше пусто
  }
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

// TrackMetaData внутри ответа UPnP — XML, вложенный в XML, поэтому дважды экранирован:
// реальные байты содержат буквально "&lt;upnp:albumArtURI&gt;URL&lt;/upnp:albumArtURI&gt;",
// не настоящие "<"/">". Полноценный XML-парсер тут так же не нужен, как и JSON-парсер выше
// (extractHexField) — просто ищем открывающий маркер по имени тега, значение — до ближайшего
// следующего "&lt;" (начала любого закрывающего тега)
static void extractEscapedXmlTag(const String& payload, const char* tagName, char* out, size_t outMax) {
  out[0] = '\0';
  String openMarker = String(tagName) + "&gt;";
  int start = payload.indexOf(openMarker);
  if (start < 0) {
    return;
  }
  start += openMarker.length();
  int end = payload.indexOf("&lt;", start);
  if (end < 0 || end < start) {
    return;
  }
  size_t len = end - start;
  if (len >= outMax) {
    len = outMax - 1;
  }
  payload.substring(start, start + len).toCharArray(out, len + 1);
}

// Обложка альбома — отдельный запрос, не тот же HTTPS API (там этого поля нет вообще, см.
// arylic_metadata.h). UPnP AVTransport — обычный HTTP (без TLS), другой порт (ARYLIC_UPNP_PORT).
// Вызывается из pollArylicMetadata() только пока трек играет — не имеет смысла опрашивать,
// когда играть нечему
static void pollArylicAlbumArt(const IPAddress& ip) {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(2000);

  String url = "http://";
  url += ip.toString();
  url += ":";
  url += String(ARYLIC_UPNP_PORT);
  url += ARYLIC_UPNP_CONTROL_PATH;
  http.begin(client, url);
  http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
  http.addHeader("SOAPACTION", "\"urn:schemas-upnp-org:service:AVTransport:1#GetPositionInfo\"");

  static const char soapBody[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body><u:GetPositionInfo xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
    "<InstanceID>0</InstanceID></u:GetPositionInfo></s:Body></s:Envelope>";

  int httpCode = http.POST((uint8_t*)soapBody, strlen(soapBody));
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    trackArtUrl[0] = '\0';
    return;
  }
  String payload = http.getString();
  http.end();

  extractEscapedXmlTag(payload, "albumArtURI", trackArtUrl, sizeof(trackArtUrl));
  // AirPlay/Apple Music отдаёт буквально строку "un_known" вместо ссылки (проверено live,
  // см. project_arylic_airplay_no_metadata в памяти) — это не URL, прятать как отсутствие
  if (strcmp(trackArtUrl, "un_known") == 0) {
    trackArtUrl[0] = '\0';
  }
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
    reachable = false;
    trackPlaying = false; // Та же логика, что и для Mega ниже — не знаем, играет ли, считаем что нет
    trackArtUrl[0] = '\0';
    trackSourceName[0] = '\0';
    megaLinkSendArylicStatus(false);
    // Не знаем, играет ли Arylic на самом деле, раз до него не достучаться — безопаснее
    // считать, что не играет (иначе Mega может застрять в режиме Now Playing/Streamer
    // навсегда, если Arylic пропал из сети посреди воспроизведения)
    megaLinkSendPlayState(false);
    // Возможно IP сменился (новый DHCP-лиз) — на следующем опросе резолвим mDNS-имя заново.
    // Ручной override (manualIpSet) это не затрагивает — см. resolveArylicIp()
    invalidateArylicIp();
    return;
  }
  reachable = true;
  megaLinkSendArylicStatus(true);

  String payload = http.getString();
  http.end();

  bool playing = payload.indexOf("\"status\":\"play\"") >= 0;
  megaLinkSendPlayState(playing);
  trackPlaying = playing;
  if (!playing) {
    // Не играет (pause/stop/idle) — метадату не шлём, PLAY:0 выше уже сказал Mega всё,
    // что нужно для выхода из Now Playing/возврата предыдущего Source
    trackArtUrl[0] = '\0';
    trackSourceName[0] = '\0';
    return;
  }

  // curpos/totlen обновляем независимо от того, распарсятся ли Title/Artist ниже —
  // это отдельные поля того же ответа, прогресс-бар веб-страницы не должен зависеть
  // от успеха разбора текста трека
  trackPosMs = extractLongField(payload, "\"curpos\":\"");
  trackLenMs = extractLongField(payload, "\"totlen\":\"");
  trackCaptureMillis = millis();

  // Обложка — отдельный запрос (другой порт/протокол, см. pollArylicAlbumArt) — вызываем
  // только пока реально играет, тем же принципом, что и curpos/totlen выше
  pollArylicAlbumArt(resolveArylicIp());

  // Источник (Spotify/AirPlay/...) — тоже независимо от того, распарсятся ли Title/Artist
  // ниже: AirPlay на этом устройстве не отдаёт их вообще (см. computeSourceName), но само
  // название источника получить можно всегда
  computeSourceName(payload);
  megaLinkSendSource(trackSourceName);

  char title[32];
  char artist[32];
  extractHexField(payload, "\"Title\":\"", title, sizeof(title));
  extractHexField(payload, "\"Artist\":\"", artist, sizeof(artist));

  if (title[0] == '\0' && artist[0] == '\0') {
    Serial.println("[arylic] играет, но Title/Artist не найдены в ответе — сырой ответ:");
    Serial.println(payload);
    // Без этого trackText оставался бы текстом ПРЕДЫДУЩЕГО трека (static-буфер, никто его
    // не очищал в этой ветке) — вводит в заблуждение, будто метадата и правда пришла.
    // AirPlay не отдаёт метадату вообще, ни у нас, ни в родном приложении производителя
    // (проверено live, см. project_arylic_airplay_no_metadata в памяти) — но название
    // источника уже отправлено выше (megaLinkSendSource), этого тут достаточно
    trackText[0] = '\0';
    return;
  }

  char combined[MEGA_LINK_META_MAX_LEN + 1];
  if (artist[0] && title[0]) {
    snprintf(combined, sizeof(combined), "%s - %s", artist, title);
  } else {
    snprintf(combined, sizeof(combined), "%s%s", artist, title);
  }

  // Отдельная, более длинная копия для веб-страницы (см. trackText в arylic_metadata.h) —
  // combined уже обрезан до MEGA_LINK_META_MAX_LEN для Mega, так что заново форматируем
  // без этого ограничения, а не переиспользуем combined
  if (artist[0] && title[0]) {
    snprintf(trackText, sizeof(trackText), "%s - %s", artist, title);
  } else {
    snprintf(trackText, sizeof(trackText), "%s%s", artist, title);
  }

  megaLinkSendMetadata(combined);
  Serial.print("[arylic] ");
  Serial.println(combined);
}
