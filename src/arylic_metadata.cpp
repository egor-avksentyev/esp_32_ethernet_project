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
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// Всё изменяемое состояние ниже пишет фоновая задача (см. arylicMetadataBegin(), крутится на
// втором ядре) и читает основной loop() (web_control.cpp — геттеры вызываются из обработчиков
// HTTP-запросов). Одним mutex'ом защищаем все поля разом — критические секции всегда короткие
// (просто присвоение/чтение), сами сетевые запросы (HTTPClient/MDNS) идут ВНЕ лока, чтобы не
// заставлять веб-сервер ждать TLS-хендшейк
static SemaphoreHandle_t stateMutex = nullptr;

struct MutexGuard {
  SemaphoreHandle_t handle;
  explicit MutexGuard(SemaphoreHandle_t h) : handle(h) { xSemaphoreTake(handle, portMAX_DELAY); }
  ~MutexGuard() { xSemaphoreGive(handle); }
};

// ПОПЫТКА объединить getPlayerStatus (опрос) и setPlayerCmd (команды с кнопок) на один общий
// TLS-канал с мьютексом была откачена (см. git-историю) — по факту стало ХУЖЕ (3-4с вместо
// 2с): похоже, Arylic не держит соединение живым между запросами (закрывает сразу после
// ответа), так что общий мьютекс только добавлял ожидание друг друга поверх двух и так
// небыстрых хендшейков, не давая обещанного переиспользования. Раздельные static-клиенты
// ниже (по одному на опрос и на команды) хотя бы не мешают друг другу

// Резолвит ARYLIC_MDNS_HOSTNAME через mDNS (см. config.h — имя найдено live через
// `dns-sd -B _linkplay._tcp local.`, привязано к устройству через его MAC, не к текущему
// IP). Кэшируется, пока запросы к Arylic проходят успешно — перерезолвливается заново
// только если опрос получит отказ, а не на каждый опрос: сам mDNS-запрос — блокирующий
// (см. MDNS.queryHost()), незачем платить эту цену каждые ARYLIC_POLL_INTERVAL_MS, когда
// IP скорее всего не менялся
static IPAddress arylicIp;
static bool arylicIpKnown = false;
static bool reachable = false;

// Ручной ввод с веб-страницы (setArylicIpOverride()) — пока задан, имеет приоритет над
// mDNS/статическим фолбэком целиком, resolveArylicIp() до них даже не доходит
static IPAddress manualIp;
static bool manualIpSet = false;

// Найдено живьём (2026-10-21): жёстко зашитый в config.h ARYLIC_IP_OCTETS — это адрес Arylic на
// КОНКРЕТНОЙ домашней сети на момент прошивки; сменит роутер IP по DHCP (или устройство переедет
// на другую сеть) — константа молча станет мёртвой ровно так же, как это уже один раз случилось
// (была .139, реальный адрес оказался .132, см. git-историю этой правки). Вместо повторного ручного
// подбора при каждой такой смене — переживающий перезагрузки кэш ПОСЛЕДНЕГО РЕАЛЬНО РЕЗОЛВЛЕННОГО
// через mDNS адреса (NVS, namespace "arylic"): сам себя обновляет на каждый успешный mDNS-запрос,
// подстраивается под любую сеть автоматически. Зашитая константа остаётся только как последний
// резерв — на случай первого в жизни запуска на новой сети, когда mDNS ещё ни разу не ответил и
// в NVS писать было нечего (в этом случае она всё равно, скорее всего, не достучится, но это не
// хуже, чем просто ничего не делать)
static void savePersistedArylicIp(IPAddress ip) {
  Preferences prefs;
  prefs.begin("arylic", false);
  prefs.putUInt("ip", (uint32_t)ip);
  prefs.end();
}

// Возвращает 0.0.0.0, если в NVS ещё ничего не сохранено (первый запуск / полный стереть-всё сброс)
static IPAddress loadPersistedArylicIp() {
  Preferences prefs;
  prefs.begin("arylic", true);
  uint32_t raw = prefs.getUInt("ip", 0);
  prefs.end();
  return IPAddress(raw);
}

// Прогресс трека для веб-страницы (см. arylic_metadata.h) — отдельная копия от того, что
// уходит на Mega (там своя, короче, MEGA_LINK_META_MAX_LEN)
static char trackText[64] = "";
static long trackPosMs = 0;
static long trackLenMs = 0;
static unsigned long trackCaptureMillis = 0;
static bool trackPlaying = false;
static char trackArtUrl[160] = "";
static char trackSourceName[24] = "";
static int currentVolume = -1;

// Оптимистичный оверрайд play/pause собственной кнопкой (см. arylicNotifyOnepausePressed() в
// arylic_metadata.h за подробным объяснением, почему это нужно и в чём ограничение) —
// playOverrideMode запоминает "mode" на момент нажатия, чтобы понять, когда оверрайд больше
// не в тему (сменился источник); lastSeenMode обновляется на каждом опросе, чтобы
// arylicNotifyOnepausePressed() (вызывается из другого потока/контекста, не из опроса) знал
// актуальный mode прямо в момент нажатия, а не ждал следующего опроса
static bool playOverrideActive = false;
static bool playOverrideValue = false;
static char playOverrideMode[8] = "";
static char lastSeenMode[8] = "";
// Дебаунс инвалидации по смене mode — требуем НЕСКОЛЬКО опросов подряд с другим mode, а не один
// (см. применение в pollArylicMetadataOnce()): при опросе раз в ARYLIC_POLL_INTERVAL_MS (сейчас
// 500мс) единичный "дребезг" в поле mode (если Arylic вдруг на миг отдаст что-то другое, не
// меняя реального источника) иначе сразу сбрасывал бы оверрайд — реле откатывалось бы обратно
// на (сломанный) "status" через долю секунды после того, как только что корректно среагировало
static uint8_t modeChangeStreak = 0;
#define PLAY_OVERRIDE_INVALIDATE_STREAK 3

bool arylicTrackIsPlaying() { MutexGuard g(stateMutex); return trackPlaying; }
String arylicTrackText() { MutexGuard g(stateMutex); return String(trackText); }
long arylicTrackPosMs() { MutexGuard g(stateMutex); return trackPosMs; }
long arylicTrackLenMs() { MutexGuard g(stateMutex); return trackLenMs; }
unsigned long arylicTrackAgeMs() { MutexGuard g(stateMutex); return millis() - trackCaptureMillis; }
String arylicTrackArtUrl() { MutexGuard g(stateMutex); return String(trackArtUrl); }
String arylicTrackSourceName() { MutexGuard g(stateMutex); return String(trackSourceName); }
int arylicCurrentVolume() { MutexGuard g(stateMutex); return currentVolume; }

bool arylicIsReachable() {
  MutexGuard g(stateMutex);
  return reachable;
}

String arylicCurrentIp() {
  MutexGuard g(stateMutex);
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
    {
      MutexGuard g(stateMutex);
      manualIpSet = false;
      arylicIpKnown = false; // на следующем опросе снова резолвим через mDNS с чистого листа
    }
    Serial.println("[arylic] ручной IP снят, возвращаюсь к mDNS");
    return;
  }
  IPAddress parsed;
  if (!parsed.fromString(ip)) {
    Serial.print("[arylic] некорректный IP с веб-страницы: ");
    Serial.println(ip);
    return;
  }
  {
    MutexGuard g(stateMutex);
    manualIp = parsed;
    manualIpSet = true;
  }
  Serial.print("[arylic] IP задан вручную: ");
  Serial.println(parsed);
}

// Читает manualIp/arylicIpKnown/arylicIp под локом (быстро), но сам mDNS-запрос (до 2с
// блокировки) — вне лока: это единственная функция, которую параллельно дёргают и фоновая
// задача опроса, и обработчики /playback, /volume (arylicSendPlayerCommand()/arylicSetVolume()
// шлют команды из основного loop()) — если бы mDNS-запрос шёл под локом, кнопка Play/Pause
// могла бы зависнуть на те же 2с, ровно то, чего мы стараемся избежать
static IPAddress resolveArylicIp() {
  bool manual, known;
  IPAddress manualCopy, cachedCopy;
  {
    MutexGuard g(stateMutex);
    manual = manualIpSet;
    manualCopy = manualIp;
    known = arylicIpKnown;
    cachedCopy = arylicIp;
  }
  if (manual) {
    return manualCopy;
  }
  if (known) {
    return cachedCopy;
  }

  IPAddress resolved = MDNS.queryHost(ARYLIC_MDNS_HOSTNAME, 2000);
  IPAddress newIp;
  if (resolved != IPAddress((uint32_t)0)) {
    newIp = resolved;
    Serial.print("[arylic] mDNS: ");
    Serial.print(ARYLIC_MDNS_HOSTNAME);
    Serial.print(".local -> ");
    Serial.println(newIp);
    savePersistedArylicIp(newIp); // см. комментарий у loadPersistedArylicIp() выше
  } else {
    IPAddress persisted = loadPersistedArylicIp();
    if (persisted != IPAddress((uint32_t)0)) {
      newIp = persisted;
      Serial.print("[arylic] mDNS-резолв не удался, использую последний известный адрес: ");
      Serial.println(newIp);
    } else {
      newIp = IPAddress(ARYLIC_IP_OCTETS);
      Serial.print("[arylic] mDNS-резолв не удался, ничего не сохранено — последний резерв из "
                    "config.h: ");
      Serial.println(newIp);
    }
  }
  {
    MutexGuard g(stateMutex);
    arylicIp = newIp;
    arylicIpKnown = true; // не долбим mDNS каждый опрос и на фолбэке — тоже до следующего сбоя
  }
  return newIp;
}

static String arylicUrl() {
  String url = "https://";
  url += resolveArylicIp().toString();
  url += "/httpapi.asp?command=getPlayerStatus";
  return url;
}

// Общий отправитель команд управления (play/pause/next/prev/volume) — один запрос,
// без ожидания ответа с данными (Arylic отвечает просто "OK"). Вызывается по действию
// пользователя на веб-странице (handlePlayback()/handleVolume() в web_control.cpp).
//
// УСТАРЕЛО (2026-10-25): комментарий раньше утверждал "всегда из основного loop()-потока —
// static здесь безопасен без мьютекса" — это давно не так: handlePlayback()/handleVolume()
// живут на webControlTaskFn, а "power"-путь (applyWebPowerState()) — на arylicPollTask (см.
// arylicRequestOnepauseAsync() выше в этом файле/в .h), то есть два РАЗНЫХ потока могут
// дёрнуть этот static `client` не строго по очереди. На практике это редкое по времени
// совпадение (клик play/pause ровно в момент включения/выключения питания), не разобрано
// мьютексом — известный, но пока не воспроизведённый живьём риск, не трогать мимоходом,
// разбираться отдельно если даст о себе знать (см. memory project_companion_ws_nonblocking_send_fix
// за похожим классом проблемы, решённым для другого пути)
// Лог connected()-до-запроса — чтобы по факту (не гадая) увидеть, реально ли Arylic держит
// соединение живым между кликами, или каждый раз всё равно платим за хендшейк заново
static bool sendArylicCommand(const String& command) {
  static WiFiClientSecure client;
  bool wasConnected = client.connected();
  Serial.print("[arylic] команда, соединение ");
  Serial.println(wasConnected ? "переиспользовано" : "новое (хендшейк)");
  client.setInsecure();
  // Найдено живьём (2026-10-20): WiFiClientSecure's TLS-хендшейк живёт СВОИМ отдельным
  // таймаутом (setHandshakeTimeout(), не setTimeout()/http.setTimeout() — те только для уже
  // установленного соединения), и по умолчанию это 120000мс (WiFiClientSecure.cpp). Эта
  // функция вызывается СИНХРОННО из основного loop()-потока (см. комментарий выше) — если
  // хендшейк подвиснет (не TCP-отказ, а именно залипание на уровне TLS — правдоподобно при
  // слабом Wi-Fi), это стопорит вообще всё: WS к кнопу и браузеру, UART-реле к Меге, HTTP —
  // на целых 2 минуты, без каких-либо признаков жизни устройства. Именно это пользователь
  // описал как "зависает, помогает только выключить-включить питание". Секунды, не
  // миллисекунды — тот же API, что setTimeout(uint32_t seconds).
  // UPDATE (тот же день, живой лог): 2с оказалось МАЛО для этого реального Arylic — рукопожатие
  // стало проваливаться буквально КАЖДЫЙ раз (видно в логе — "start_ssl_client: -1" на каждой
  // попытке, с самой первой, на свежей после загрузки куче). Увеличил до 8с — достаточно, чтобы
  // настоящее рукопожатие успевало завершиться, но всё ещё в разы лучше дефолтных 120с.
  client.setHandshakeTimeout(8);

  HTTPClient http;
  http.setTimeout(2000);
  String url = "https://";
  url += resolveArylicIp().toString();
  url += "/httpapi.asp?command=";
  url += command;
  http.begin(client, url);
  unsigned long start = millis();
  int httpCode = http.GET();
  Serial.print("[arylic] команда заняла мс: ");
  Serial.println(millis() - start);
  String resp = http.getString();
  http.end();
  // Найдено живьём (2026-10-20) — настоящая причина утечки кучи: HTTPClient::end() зовёт
  // client->stop() ТОЛЬКО если connected() истинно (framework's HTTPClient.cpp's disconnect()).
  // Если рукопожатие провалилось — client ещё не "connected", http.end() молча НИЧЕГО не
  // чистит, а stop_ssl_socket() (framework's ssl_client.cpp, с собственным комментарием "avoid
  // memory leak if ssl connection attempt failed") просто никогда не вызывается. На каждую
  // неудачную попытку — реальная утечка mbedTLS-буферов. Зовём stop() сами, безусловно,
  // независимо от того, что решил сделать http.end().
  client.stop();
  return httpCode == HTTP_CODE_OK && resp == "OK";
}

bool arylicSendPlayerCommand(const char* command) {
  return sendArylicCommand(String("setPlayerCmd:") + command);
}

bool arylicSetVolume(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  return sendArylicCommand(String("setPlayerCmd:vol:") + percent);
}

bool arylicSeek(long posMs) {
  if (posMs < 0) posMs = 0;
  return sendArylicCommand(String("setPlayerCmd:seek:") + (posMs / 1000));
}

// Свежий mDNS-запрос напрямую (не через resolveArylicIp() — та кэширует и в первую очередь
// смотрит на manualIp/статический фолбэк из config.h, ни то ни другое не годится, если система
// физически переехала в другую сеть с прошлого опроса). Фолбэк на resolveArylicIp() — только
// если живой mDNS-запрос прямо сейчас не ответил, лучше какой-то шанс достучаться, чем никакого
// Найдено живьём (2026-10-21): даже с рабочим persisted-фолбэком (см. resolveArylicIp()) один
// конкретный запуск этой функции иногда просто не долетает до Arylic — тот же класс дребезга
// mDNS/TLS, что и у обычного опроса (см. ARYLIC_UNREACHABLE_STREAK ниже), только здесь не было
// НИ ОДНОЙ попытки повтора. Для обычного опроса единичный сбой неважен — следующий тик через
// 500мс сам всё поправит; здесь же "следующего тика" просто не будет — ESP.restart() уходит
// через секунду после вызова, так что единственный шанс всё равно выполнить сброс — это именно
// сейчас. Два прохода (каждый — свой свежий mDNS-запрос, не один закэшированный IP на оба) с
// небольшим зазором — минимальная защита конкретно для этого редкого, но важного места.
void arylicTriggerFactoryReset() {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (attempt > 0) {
      Serial.println("[arylic] заводской сброс: первая попытка не удалась, пробую ещё раз");
      delay(300);
    }
    IPAddress ip = MDNS.queryHost(ARYLIC_MDNS_HOSTNAME, 2000);
    if (ip == IPAddress((uint32_t)0)) {
      Serial.println("[arylic] заводской сброс: свежий mDNS не ответил, беру резолвленный/статический IP");
      ip = resolveArylicIp();
    } else {
      Serial.print("[arylic] заводской сброс: mDNS -> ");
      Serial.println(ip);
    }
    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(8); // see sendArylicCommand() above — bumped 2->8, see its comment
    client.setTimeout(3000);
    if (!client.connect(ip, 443)) {
      Serial.println("[arylic] заводской сброс: не удалось подключиться");
      client.stop(); // see sendArylicCommand() above — don't rely on the destructor for this
      continue;
    }
    // Пишем запрос и сразу рвём соединение, не читая ответ — сам Arylic отвечает не сразу
    // (см. живой тест: ~9с до "OK"), а результат тут всё равно не проверяется и не показывается
    client.print(String("GET /httpapi.asp?command=restoreToDefault HTTP/1.1\r\n") +
                 "Host: " + ip.toString() + "\r\n" +
                 "Connection: close\r\n\r\n");
    client.flush();
    client.stop();
    Serial.println("[arylic] заводской сброс: запрос отправлен, ответа не ждём");
    return;
  }
  Serial.println("[arylic] заводской сброс: обе попытки не удались, пропускаю");
}

static String toHexUpper(const String& input) {
  static const char hexDigits[] = "0123456789ABCDEF";
  String out;
  out.reserve(input.length() * 2);
  for (size_t i = 0; i < input.length(); i++) {
    uint8_t b = (uint8_t)input[i];
    out += hexDigits[(b >> 4) & 0xF];
    out += hexDigits[b & 0xF];
  }
  return out;
}

void arylicProvisionWifiIfInSetupMode(const char* homeSsid, const char* homePassword) {
  Serial.println("[arylic] проверяю, не стоит ли Arylic сейчас в режиме настройки Wi-Fi...");
  int n = WiFi.scanNetworks();
  bool arylicSetupSeen = false;
  bool homeSeen = false;
  int homeChannel = 1; // дефолт, если домашняя сеть не попадётся в этом же скане
  wifi_auth_mode_t homeEncType = WIFI_AUTH_WPA2_PSK; // дефолт — см. ниже про безопасный фолбэк
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == ARYLIC_SETUP_AP_SSID) {
      arylicSetupSeen = true;
    }
    if (WiFi.SSID(i) == String(homeSsid)) {
      homeChannel = WiFi.channel(i);
      homeEncType = WiFi.encryptionType(i);
      homeSeen = true;
    }
  }
  WiFi.scanDelete();

  if (!arylicSetupSeen) {
    Serial.println("[arylic] его точка настройки не видна — скорее всего, не сбрасывался заодно с ESP32, пропускаю этот шаг");
    return;
  }
  Serial.println("[arylic] вижу его временную точку настройки — пробую передать домашние креды напрямую, без 4stream");
  if (!homeSeen) {
    Serial.println("[arylic] домашняя сеть не попалась в этом же скане — использую дефолт (канал 1, WPA2PSK/AES)");
  }

  bool isOpen = homeSeen && homeEncType == WIFI_AUTH_OPEN;
  const char* authStr = isOpen ? "OPEN" : "WPA2PSK";
  const char* encryStr = isOpen ? "NONE" : "AES";

  // Временная точка Arylic в режиме настройки всегда открытая (без пароля)
  WiFi.begin(ARYLIC_SETUP_AP_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < ARYLIC_PROVISION_JOIN_TIMEOUT_MS) {
    delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[arylic] не успел присоединиться к его точке настройки за отведённое время, пропускаю");
    return;
  }

  IPAddress gateway = WiFi.gatewayIP();
  if (gateway == IPAddress((uint32_t)0)) {
    gateway = IPAddress(ARYLIC_PROVISION_AP_GATEWAY_OCTETS);
  }
  Serial.print("[arylic] присоединился к его точке настройки, гейтвей: ");
  Serial.println(gateway);

  String url = "https://" + gateway.toString() + "/httpapi.asp?command=wlanConnectApEx:ssid=" +
               toHexUpper(String(homeSsid)) + ":ch=" + String(homeChannel) + ":auth=" + authStr +
               ":encry=" + encryStr + ":pwd=" + toHexUpper(String(homePassword)) + ":chext=1";

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(8); // see sendArylicCommand() above — bumped 2->8, see its comment
  HTTPClient http;
  http.setTimeout(5000);
  http.begin(client, url);
  int httpCode = http.GET();
  String resp = http.getString();
  http.end();
  client.stop(); // see sendArylicCommand() above — http.end() skips this on a failed handshake
  Serial.print("[arylic] wlanConnectApEx -> код ");
  Serial.print(httpCode);
  Serial.print(", ответ: ");
  Serial.println(resp);
  // Не опрашиваем wlanGetConnectState — живой тест 2026-10-18 показал, что временная точка
  // Arylic обычно гаснет в процессе переключения на домашнюю сеть раньше, чем успеваешь
  // получить вразумительный ответ оттуда. Дальше просто доверяем команде; вызывающая сторона
  // (handleSave() в wifi_provisioning.cpp) следом уходит в ESP.restart(), что само по себе
  // сбросит это временное STA-подключение
}

void arylicNotifyOnepausePressed() {
  MutexGuard g(stateMutex);
  // Оверрайд нужен ТОЛЬКО для AirPlay (mode "1") — там "status" подтверждённо не меняется на
  // паузе (см. project_arylic_airplay_no_metadata в памяти). Для остальных источников (Spotify
  // Connect и т.д.) "status" отражает реальность нормально — раньше оверрайд ставился
  // безусловно для любого источника и оставался активным НАВСЕГДА, пока не сменится mode
  // (см. инвалидацию ниже, в pollArylicMetadataOnce()): если нажать нашу же кнопку play/pause
  // на Spotify "на всякий случай", оверрайд начинал подменять собой честный "status" и
  // переставал замечать реальные изменения состояния, сделанные НЕ через эту кнопку (телефон,
  // другое приложение) — реле переставало реагировать на них. Не только "не помогает" для
  // надёжных источников, а активно вредит
  if (strcmp(lastSeenMode, "1") != 0) {
    return;
  }
  bool currentBelief = playOverrideActive ? playOverrideValue : trackPlaying;
  playOverrideActive = true;
  playOverrideValue = !currentBelief;
  strncpy(playOverrideMode, lastSeenMode, sizeof(playOverrideMode) - 1);
  playOverrideMode[sizeof(playOverrideMode) - 1] = '\0';
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
//
// Пишет в переданный буфер, а не прямо в trackSourceName — эта функция вызывается из фоновой
// задачи опроса ДО того, как результат публикуется под локом (см. pollArylicMetadataOnce()),
// чтобы не держать mutex на всё время разбора строки
static void computeSourceName(const String& payload, char* out, size_t outMax) {
  char vendor[48];
  extractStringField(payload, "\"vendor\":\"", vendor, sizeof(vendor));
  // Найдено живьём (2026-10-20, через KnobControl): Arylic иногда отдаёт буквально
  // "vendor":"-" для настоящей Spotify Connect сессии (не просто пустую строку) — похоже,
  // собственный сентинел LinkPlay "неизвестно", а не осознанное имя источника. Раньше это
  // бралось буквально и показывалось как есть ("-") — на кругом экране кнопа одинокий дефис
  // в центре обложки и отдельной строкой ниже выглядел как необъяснимые "точки-артефакты",
  // пока не сверили с live `curl` сюда же. Приравниваем к "вендор неизвестен", как и пустую
  // строку — ровно то же самое по сути, другая запись
  if (strcmp(vendor, "-") == 0) {
    vendor[0] = '\0';
  }
  if (vendor[0] != '\0') {
    char* colon = strchr(vendor, ':');
    if (colon) {
      *colon = '\0';
    }
    if (strcasecmp(vendor, "spotify") == 0) {
      snprintf(out, outMax, "Spotify");
    } else if (strcasecmp(vendor, "tidal") == 0) {
      snprintf(out, outMax, "Tidal");
    } else if (strcasecmp(vendor, "deezer") == 0) {
      snprintf(out, outMax, "Deezer");
    } else if (strcasecmp(vendor, "qobuz") == 0) {
      snprintf(out, outMax, "Qobuz");
    } else if (strcasecmp(vendor, "amazon") == 0) {
      snprintf(out, outMax, "Amazon Music");
    } else {
      snprintf(out, outMax, "%s", vendor);
      out[0] = toupper(out[0]);
    }
    return;
  }

  char mode[8];
  extractStringField(payload, "\"mode\":\"", mode, sizeof(mode));
  if (strcmp(mode, "1") == 0) {
    snprintf(out, outMax, "AirPlay"); // подтверждено live 2026-09-12
  } else if (strcmp(mode, "2") == 0) {
    snprintf(out, outMax, "DLNA");
  } else if (strcmp(mode, "40") == 0) {
    snprintf(out, outMax, "Line-In");
  } else if (strcmp(mode, "41") == 0) {
    snprintf(out, outMax, "Bluetooth");
  } else if (strcmp(mode, "43") == 0) {
    snprintf(out, outMax, "Optical");
  } else {
    out[0] = '\0'; // неизвестный код — не гадаем, лучше пусто
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
    MutexGuard g(stateMutex);
    trackArtUrl[0] = '\0';
    return;
  }
  String payload = http.getString();
  http.end();

  // Разбор — в локальный буфер, без лока (строковые операции, не общее состояние);
  // публикуем в trackArtUrl одним быстрым memcpy под локом ниже
  char newArt[sizeof(trackArtUrl)];
  extractEscapedXmlTag(payload, "albumArtURI", newArt, sizeof(newArt));
  // AirPlay/Apple Music отдаёт буквально строку "un_known" вместо ссылки (проверено live,
  // см. project_arylic_airplay_no_metadata в памяти) — это не URL, прятать как отсутствие
  if (strcmp(newArt, "un_known") == 0) {
    newArt[0] = '\0';
  }

  MutexGuard g(stateMutex);
  memcpy(trackArtUrl, newArt, sizeof(trackArtUrl));
}

// Одна итерация опроса — сетевой вызов идёт на своём static-клиенте (не общем с командами,
// см. комментарий выше), публикация результата в общее состояние — короткими блоками под
// stateMutex. Раньше это было pollArylicMetadata(), вызывавшаяся прямо из loop() (см.
// arylicMetadataBegin() ниже за тем, почему теперь она крутится в отдельной задаче)
static void pollArylicMetadataOnce() {
  // static — не локальная переменная: эта функция всегда вызывается из одной и той же фоновой
  // задачи (arylicPollTask), конкурентного доступа нет, мьютекс не нужен. Даже если Arylic не
  // держит соединение между опросами (см. лог connected() в sendArylicCommand() — по нему
  // будет видно), не хуже локальной переменной, а если вдруг держит — бесплатный выигрыш
  static WiFiClientSecure client;
  client.setInsecure(); // самоподписанный сертификат Arylic — цепочку не проверяем
  client.setHandshakeTimeout(8); // see sendArylicCommand() above — bumped 2->8 AND added the
                                 // explicit stop() below, same day, same live log: 2s made
                                 // EVERY attempt fail (confirmed in the log, from the very
                                 // first one on a fresh heap) and leak on each failure via
                                 // http.end() skipping client.stop() on a failed handshake

  HTTPClient http;
  http.setTimeout(2000); // локальная сеть — секунды с запасом на "не отвечает/выключен"
  http.begin(client, arylicUrl());
  int httpCode = http.GET();
  String payload;
  if (httpCode == HTTP_CODE_OK) {
    payload = http.getString();
  }
  http.end();
  client.stop(); // see sendArylicCommand() above for why this is required, not redundant

  // Дебаунс на несколько опросов подряд — раньше даже ОДИН неудачный запрос (разовый WiFi/TLS-
  // дребезг, а не настоящая потеря связи) сразу считался "Arylic пропал", слал Mega PLAY:0 и
  // тут же инвалидировал закэшированный IP (провоцируя ещё и медленный mDNS-резолв на
  // следующем опросе, который сам может не уложиться в таймаут). Если следующий опрос (уже
  // через ARYLIC_POLL_INTERVAL_MS) при этом снова успешен — получалось короткое реле "туда-
  // сюда" без видимой причины: редкий, но реальный баг, замеченный пользователем live.
  // ARYLIC_UNREACHABLE_STREAK опросов подряд — это ~1.5с при текущем интервале 500мс,
  // достаточно, чтобы отфильтровать единичный дребезг, но не настолько долго, чтобы заметно
  // задержать честное обнаружение реальной потери связи
  static uint8_t consecutiveFailCount = 0;
  // См. применение ниже, у проверки по позиции трека — держим здесь, у остальных
  // static-счётчиков этой функции, чтобы можно было сбросить в ветке "устройство недоступно"
  // ниже (иначе устаревшее значение могло бы ложно "подтвердить" воспроизведение после
  // переподключения к другому треку/устройству)
  static long lastPlayingPosMs = -1; // -1 — ещё ни разу не видели точно играющим
  if (httpCode != HTTP_CODE_OK) {
    consecutiveFailCount++;
    Serial.print("[arylic] запрос не удался, код: ");
    Serial.println(httpCode);
    if (consecutiveFailCount < ARYLIC_UNREACHABLE_STREAK) {
      return; // ещё может быть разовым дребезгом — состояние (reachable/playing/...) не трогаем
    }

    bool wasReachable;
    {
      MutexGuard g(stateMutex);
      wasReachable = reachable;
      reachable = false;
      trackPlaying = false; // Та же логика, что и для Mega ниже — не знаем, играет ли, считаем что нет
      trackArtUrl[0] = '\0';
      trackSourceName[0] = '\0';
      trackText[0] = '\0'; // устройство недоступно целиком — не оставлять на веб-странице
      // текст/обложку ПРЕДЫДУЩЕГО трека (в отличие от паузы ниже, где это сделано намеренно)
      currentVolume = -1;
      // Возможно IP сменился (новый DHCP-лиз) — на следующем опросе резолвим mDNS-имя заново.
      // Ручной override (manualIpSet) это не затрагивает — см. resolveArylicIp()
      arylicIpKnown = false;
    }
    if (wasReachable) {
      Serial.println("[arylic] связь потеряна, не слышу Arylic");
    }
    megaLinkSendArylicStatus(false);
    // Не знаем, играет ли Arylic на самом деле, раз до него не достучаться — безопаснее
    // считать, что не играет (иначе Mega может застрять в режиме Now Playing/Streamer
    // навсегда, если Arylic пропал из сети посреди воспроизведения)
    megaLinkSendPlayState(false);
    lastPlayingPosMs = -1;
    return;
  }
  consecutiveFailCount = 0;

  bool wasReachable;
  {
    MutexGuard g(stateMutex);
    wasReachable = reachable;
    reachable = true;
  }
  if (!wasReachable) {
    Serial.print("[arylic] слышу Arylic, ");
    Serial.println(arylicUrl());
  }
  megaLinkSendArylicStatus(true);

  // Громкость усилителя — актуальна независимо от того, играет ли что-то сейчас (в отличие
  // от curpos/totlen/title ниже), поэтому обновляется тут, а не внутри блока playing
  int newVolume = (int)extractLongField(payload, "\"vol\":\"");

  bool playing = payload.indexOf("\"status\":\"play\"") >= 0;
  char currentMode[8];
  extractStringField(payload, "\"mode\":\"", currentMode, sizeof(currentMode));
  bool overrideActiveNow;
  {
    // "status" врёт для AirPlay (см. project_arylic_airplay_no_metadata в памяти) — если
    // пользователь только что нажал нашу же кнопку play/pause (arylicNotifyOnepausePressed()),
    // доверяем ЕЙ, а не этому полю, пока не сменится источник (mode)
    MutexGuard g(stateMutex);
    strncpy(lastSeenMode, currentMode, sizeof(lastSeenMode) - 1);
    lastSeenMode[sizeof(lastSeenMode) - 1] = '\0';
    if (playOverrideActive) {
      if (strcmp(currentMode, playOverrideMode) != 0) {
        // Не сбрасываем оверрайд по первому же несовпадению — см. PLAY_OVERRIDE_INVALIDATE_STREAK
        // за тем, почему нужно подряд несколько опросов с другим mode, а не один
        modeChangeStreak++;
        if (modeChangeStreak >= PLAY_OVERRIDE_INVALIDATE_STREAK) {
          playOverrideActive = false; // сменился источник — старое предположение больше не в тему
          modeChangeStreak = 0;
        } else {
          playing = playOverrideValue; // ещё в пределах дебаунса — продолжаем доверять оверрайду
        }
      } else {
        modeChangeStreak = 0;
        playing = playOverrideValue;
      }
    }
    overrideActiveNow = playOverrideActive;
  }

  // Живой пример 2026-09-17: Spotify Connect (mode 31, оверрайд тут ни при чём) один раз
  // отдал "status" не "play" на ОДИН опрос, хотя стрим реально не прерывался — LinkPlay API
  // (см. https://github.com/AndersFluur/LinkPlayApi) допускает "load" как промежуточное
  // значение status (не только "play"/"pause"/"stop") — то есть это не обязательно "вранье",
  // а честное краткое переходное состояние (буферизация и т.п.), которое сам факт
  // непрерывности звука не отражает.
  //
  // Если позиция трека (curpos) всё равно продолжила расти с прошлого раза, когда точно
  // играло — значит звук физически не мог остановиться сам по себе, "status" тут явно
  // врёт/про краткий "load", доверяем позиции. Работает только для источников, где curpos
  // вообще двигается (не AirPlay — тот идёт через оверрайд выше и сюда не попадает, см.
  // project_arylic_airplay_no_metadata в памяти).
  //
  // Раньше здесь ещё был дебаунс "N опросов подряд с не-play, прежде чем считать паузу
  // настоящей" (PLAY_STOP_DEBOUNCE_STREAK) — задумывался как вторая линия защиты на случай,
  // если позиция не помогла. Убран: переключение трека (Next/Prev) на Spotify Connect обычно
  // само занимает больше одного опроса на буферизацию — тот же класс "не play", что и
  // изначальный баг, только НАСТОЯЩИЙ разрыв, не вранье. Дебаунс держал реле/экран Streamer
  // в подвешенном состоянии на это время, а потом всё равно резко переключал туда-сюда —
  // выглядело как подтормаживание при каждом свайпе трека, чаще и заметнее, чем спасало
  long curposNow = extractLongField(payload, "\"curpos\":\"");
  if (!overrideActiveNow && !playing && lastPlayingPosMs >= 0 && curposNow > lastPlayingPosMs) {
    playing = true;
  }
  if (playing) {
    lastPlayingPosMs = curposNow;
  }
  // trackPlaying — ДО отправки PLAY: Меге, не после (раньше выставлялся ниже, за блокирующим
  // запросом обложки, см. pollArylicAlbumArt()) — иначе реальная гонка: Mega получает PLAY:1,
  // сама включается из Standby и шлёт POWER: обратно ЗНАЧИТЕЛЬНО раньше, чем этот же поток
  // успевает дойти до старого места установки trackPlaying=true (после запроса обложки).
  // applyWebPowerState() (web_control.cpp) в этот момент читает ещё старое (false) значение
  // через arylicTrackIsPlaying() и повторно жмёт onepause на уже играющий трек — раз в него
  // переключается пауза. Живьём подтверждено 2026-09-23 (пауза ровно в момент POWER:0)
  {
    MutexGuard g(stateMutex);
    trackPlaying = playing;
  }
  megaLinkSendPlayState(playing);
  if (!playing) {
    // Не играет (pause/stop/idle) — метадату не шлём, PLAY:0 выше уже сказал Mega всё,
    // что нужно для выхода из Now Playing/возврата предыдущего Source. Обложку/источник НЕ
    // чистим (в отличие от ветки "устройство недоступно" выше) — на паузе трек всё ещё тот
    // же самый, веб-странице незачем моргать пустой обложкой; она сама уберётся, как только
    // придут новые данные (следующий трек или реальное disconnect от Arylic)
    MutexGuard g(stateMutex);
    currentVolume = newVolume;
    trackPlaying = false;
    return;
  }

  // curpos/totlen обновляем независимо от того, распарсятся ли Title/Artist ниже —
  // это отдельные поля того же ответа, прогресс-бар веб-страницы не должен зависеть
  // от успеха разбора текста трека. curposNow уже извлечён выше (для проверки по позиции)
  long newPosMs = curposNow;
  long newLenMs = extractLongField(payload, "\"totlen\":\"");
  megaLinkSendPosition(newPosMs, newLenMs);

  // Обложка — отдельный запрос (другой порт/протокол, см. pollArylicAlbumArt) — вызываем
  // только пока реально играет, тем же принципом, что и curpos/totlen выше. Сама публикует
  // trackArtUrl под локом
  pollArylicAlbumArt(resolveArylicIp());

  // Источник (Spotify/AirPlay/...) — тоже независимо от того, распарсятся ли Title/Artist
  // ниже: AirPlay на этом устройстве не отдаёт их вообще (см. computeSourceName), но само
  // название источника получить можно всегда
  char sourceBuf[24];
  computeSourceName(payload, sourceBuf, sizeof(sourceBuf));
  megaLinkSendSource(sourceBuf);

  char title[32];
  char artist[32];
  extractHexField(payload, "\"Title\":\"", title, sizeof(title));
  extractHexField(payload, "\"Artist\":\"", artist, sizeof(artist));

  {
    MutexGuard g(stateMutex);
    currentVolume = newVolume;
    // trackPlaying уже выставлен выше, до megaLinkSendPlayState() — см. комментарий там
    trackPosMs = newPosMs;
    trackLenMs = newLenMs;
    trackCaptureMillis = millis();
    strncpy(trackSourceName, sourceBuf, sizeof(trackSourceName) - 1);
    trackSourceName[sizeof(trackSourceName) - 1] = '\0';
  }

  if (title[0] == '\0' && artist[0] == '\0') {
    Serial.println("[arylic] играет, но Title/Artist не найдены в ответе — сырой ответ:");
    Serial.println(payload);
    // Без этого trackText/nowPlayingText (на Mega) оставались бы текстом ПРЕДЫДУЩЕГО трека
    // (static-буферы, никто их не очищал в этой ветке) — вводит в заблуждение, будто метадата
    // и правда пришла. AirPlay не отдаёт метадату вообще, ни у нас, ни в родном приложении
    // производителя (проверено live, см. project_arylic_airplay_no_metadata в памяти) — но
    // название источника уже отправлено выше (megaLinkSendSource), этого тут достаточно.
    // Раньше megaLinkSendMetadata("") тут не вызывался вообще — если до этого играл трек С
    // метадатой (например Spotify), а следующий источник её не отдаёт (AirPlay), Mega
    // продолжала показывать СТАРОЕ название трека на Now Playing бесконечно, пока метадата
    // случайно не придёт снова
    {
      MutexGuard g(stateMutex);
      trackText[0] = '\0';
    }
    megaLinkSendMetadata("");
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
  {
    MutexGuard g(stateMutex);
    if (artist[0] && title[0]) {
      snprintf(trackText, sizeof(trackText), "%s - %s", artist, title);
    } else {
      snprintf(trackText, sizeof(trackText), "%s%s", artist, title);
    }
  }

  megaLinkSendMetadata(combined);
  Serial.print("[arylic] ");
  Serial.println(combined);
}

// См. arylicRequestOnepauseAsync() (arylic_metadata.h) за полным разбором — applyWebPowerState()
// (web_control.cpp) раньше звала arylicSendPlayerCommand("onepause") прямо с потока вызывающего
// (liveWsTaskFn для WS "cmd:power", main loop() для настоящего POWER: по UART), замораживая его
// на время TLS-таймаута (вплоть до ~10с). Теперь только ставит флаг здесь — реальный вызов
// происходит ниже, внутри arylicPollTask, никогда на чужом потоке.
static volatile bool gPendingOnepauseRequest = false;

void arylicRequestOnepauseAsync() { gPendingOnepauseRequest = true; }

static void serviceAsyncOnepauseRequest() {
  if (!gPendingOnepauseRequest) return;
  gPendingOnepauseRequest = false;
  if (arylicSendPlayerCommand("onepause")) {
    arylicNotifyOnepausePressed();
  }
}

// Фоновая задача — крутится на ядре 0 (вместе со стеком Wi-Fi/lwIP, обычная практика для
// сетевых задач на ESP32), пока loop() с веб-сервером остаётся на ядре 1 и ничем не блокируется
// этим опросом. Пауза ARYLIC_POLL_INTERVAL_MS — ПОСЛЕ каждой попытки, а не по жёсткому
// расписанию: если сам запрос завис на таймауте (до 2с), не долбим повторно сразу же следом
//
// Периодически логируем свободную кучу (ESP.getFreeHeap()) — до этого рефакторинга опрос жил
// в общем loop()-стеке и устройство периодически само перезагружалось (SW_CPU_RESET), похоже
// на нехватку стека/фрагментацию кучи от частых TLS-хендшейков (WiFiClientSecure без
// keep-alive, новый handshake на каждый опрос). Если проблема в утечке — здесь будет видно
// падающий тренд задолго до самого краша; если после переноса на отдельную задачу с большим
// стеком (см. stackSize ниже) крах повторится — по этому логу плюс дампу паники (Guru
// Meditation/Backtrace, печатается автоматически) можно будет отличить утечку от разового сбоя
static void arylicPollTask(void* /*param*/) {
  uint32_t iteration = 0;
  for (;;) {
    serviceAsyncOnepauseRequest();
    if (wifiIsConnected()) {
      pollArylicMetadataOnce();
    }
    if (++iteration % 20 == 0) { // примерно раз в 10с при ARYLIC_POLL_INTERVAL_MS=500
      Serial.print("[arylic] свободная куча: ");
      Serial.println(ESP.getFreeHeap());
    }
    vTaskDelay(pdMS_TO_TICKS(ARYLIC_POLL_INTERVAL_MS));
  }
}

void arylicMetadataInitMutex() {
  if (stateMutex == nullptr) {
    stateMutex = xSemaphoreCreateMutex();
  }
}

void arylicMetadataBegin() {
  arylicMetadataInitMutex();
  // 12288 вместо дефолтных 8192 — TLS-хендшейк (mbedTLS) внутри HTTPClient/WiFiClientSecure
  // требователен к стеку, а перезагрузки на предыдущей версии (опрос прямо в loop(), общий
  // стек с веб-сервером) похожи на нехватку именно этого
  xTaskCreatePinnedToCore(arylicPollTask, "arylic_poll", 12288, nullptr, 1, nullptr, 0);
}
