#pragma once

// ============================================================================
// config.h — все константы проекта в одном месте (по образцу hardware_settings.h
// в прошивке Mega): пины, протокол UART к Mega, адрес Arylic, тайминги.
// ============================================================================

#include <Arduino.h>

// --- UART к Mega (mega_link.h/.cpp) ---
// Используем аппаратный UART2 (Serial2 на ESP32) — UART0 занят USB/монитором и логом,
// UART1 на многих платах занят SPI flash. С 2026-09-21 линия двусторонняя: ESP32 TX -> Mega
// RX2 (как и раньше) и Mega TX2 -> ESP32 RX через готовый модуль level shifter (Mega — 5V
// логика, GPIO этого ESP32 не 5V-толерантны) — см. mega_link.h за протоколом POWER:/TEMP:/VOLT:
#define MEGA_LINK_TX_PIN 17
#define MEGA_LINK_RX_PIN 16
#define MEGA_LINK_BAUD 115200
#define MEGA_LINK_META_MAX_LEN 40 // Максимум символов в строке "META:" — под однострочный показ на OLED

// --- ОТДЕЛЬНЫЙ UART под попиксельное зеркало OLED (frame_mirror.h/.cpp). Аппаратный UART1
// (Serial1) — свободен, если
// явно задать пины через begin(baud, config, rx, tx) (стандартные пины UART1 на многих платах
// заняты SPI flash, но ESP32 умеет перенаправить UART на любой GPIO через матрицу выводов).
// TX не нужен — канал строго Mega->ESP32, поэтому FRAME_MIRROR_TX_PIN не задействован (-1)
//
// 2026-10-21: было 34 (safe на классическом ESP32) — после переезда на
// Waveshare ESP32-S3-DEV-KIT-N16R8 GPIO33-37 заняты под Octal PSRAM (специфика именно R8-
// варианта с восемью мегабайтами PSRAM, на quad-PSRAM платах было бы иначе) — сменено на
// свободный GPIO4, провод перевешан физически на эту ногу.
#define FRAME_MIRROR_RX_PIN 4
#define FRAME_MIRROR_BAUD 500000 // Должно совпадать с FRAME_MIRROR_BAUD в hardware_settings.h Mega-репозитория

// --- Веб-страница управления (web_control.h/.cpp) ---
#define WEB_SERVER_PORT 80

// --- Постоянное соединение под удержание Up/Down (motor_ws.h/.cpp) — см. подробное объяснение
// там же за тем, почему не переиспользуется обычный /cmd (WebServer.h/handleCmd()) ---
#define MOTOR_WS_PORT 81
// Как часто ESP32 сам шлёт очередной CMD:U/D на Mega, пока соединение открыто и держится
// кнопка — не привязано к сети вообще, whole repeat идёт локально на ESP32
#define MOTOR_WS_REPEAT_MS 80

// --- Постоянный push статуса/трека/доступности Arylic на веб-страницу (web_control.cpp,
// liveWsBroadcastPoll()) — вместо того, чтобы браузер сам стучался на /status (1.5с),
// /track (500мс) и /arylic-status (500мс) по отдельности (каждый — новое TCP-соединение,
// WebServer.h не поддерживает keep-alive), ESP32 сам рассылает то же самое по одному
// постоянному WebSocket-соединению. HTTP-эндпоинты остаются как были — клиент падает
// обратно на них, если WebSocket недоступен (см. liveDataFresh() в PAGE_HTML) ---
#define LIVE_WS_PORT 82
// Тот же период, что был у самого частого из трёх опросов (/track, /arylic-status) — не
// нужно рассылать чаще, странице этого не требуется
#define LIVE_WS_PUSH_INTERVAL_MS 500

// --- Метадата Arylic (arylic_metadata.h/.cpp) ---
// Основной способ найти Arylic — mDNS-имя устройства (не зависит от того, какой IP ему в
// очередной раз выдаст DHCP роутера). Найдено live через `dns-sd -B _linkplay._tcp local.`
// (2026-09-10) — своё для каждого физического устройства (образовано из MAC), поменяется,
// если Arylic заменят на другой физический блок — тогда пересмотреть тем же способом
#define ARYLIC_MDNS_HOSTNAME "SoundSystem-D3F4" // без ".local" — его добавляет MDNS.queryHost()
// Фолбэк на случай, если mDNS-запрос не ответил (имя ещё не разошлось по сети, помеха и т.п.) —
// поправь вручную, если он в итоге устареет И mDNS-резолв тоже почему-то перестанет работать
// одновременно (маловероятно, но не невозможно).
// Обновлено 2026-10-21: было 192.168.1.139 — устарело (DHCP перевыдал другой адрес), подтверждено
// живым логом в тот же день (свежий mDNS внутри resolveArylicIp() нашёл реальный .132, пока
// 3 HTTPS-попытки подряд проваливались именно на старый .139). Этот же стейл-фолбэк — вероятная
// причина, почему arylicTriggerFactoryReset() иногда молча не доходит до Arylic: та функция
// фолбэкается сюда без повторов, если единственный живой mDNS-запрос (2с, без ретраев) не успел
// ответить именно в момент сброса.
#define ARYLIC_IP_OCTETS 192, 168, 1, 132
// Имя временной точки доступа, которую Arylic сам поднимает после заводского сброса (режим
// настройки Wi-Fi) — найдено live 2026-10-18: совпадает с "дружественным именем" устройства
// (тем же, что отдаёт getStatusEx как DeviceName), но с ПРОБЕЛОМ, а не дефисом, как в
// ARYLIC_MDNS_HOSTNAME (тот дефис — требование mDNS-имён, сама точка доступа этому не
// подчиняется). Если Arylic переименуют (через 4stream) или заменят на другое физическое
// устройство — поправить здесь вручную, автоматического вывода из ARYLIC_MDNS_HOSTNAME нет
// специально (замена дефиса на пробел — не выведенное правило, а разовое наблюдение по одному
// устройству, пытаться обобщать его было бы гаданием)
#define ARYLIC_SETUP_AP_SSID "SoundSystem D3F4"
// Сколько ждём присоединения к этой временной точке доступа, прежде чем сдаться и просто
// продолжить обычный провижининг ESP32 (см. arylicProvisionWifiIfInSetupMode() —
// best-effort шаг, не должен надолго задерживать основной поток настройки)
#define ARYLIC_PROVISION_JOIN_TIMEOUT_MS 8000UL
// Гейтвей LinkPlay-устройств в режиме собственной точки доступа — стандартный для этого
// семейства адрес (подтверждено live на данном устройстве). Используется только как фолбэк,
// если WiFi.gatewayIP() после присоединения почему-то вернёт 0.0.0.0 — в норме берём
// реальный адрес, выданный самим устройством по DHCP, не этот хардкод
#define ARYLIC_PROVISION_AP_GATEWAY_OCTETS 10, 10, 10, 254
// Опрос идёт в отдельной FreeRTOS-задаче (arylicMetadataBegin(), ядро 0), не в loop() — не
// блокирует веб-сервер, поэтому можно опрашивать агрессивно. Ограничитель тут — не отзывчивость
// страницы, а нагрузка на сам Arylic (TLS-хендшейк без keep-alive на каждый опрос) и на CPU/Wi-Fi
// ESP32; 500 — разумный компромисс, если начнутся сбои связи с Arylic — увеличить
#define ARYLIC_POLL_INTERVAL_MS 500
// Сколько НЕУДАЧНЫХ опросов подряд считать реальной потерей связи (не разовым WiFi/TLS-
// дребезгом) — см. подробное объяснение в pollArylicMetadataOnce() (arylic_metadata.cpp).
// 3 опроса при интервале 500мс — это ~1.5с, отфильтровывает единичный сбой, не задерживая
// заметно реальное обнаружение отключения
#define ARYLIC_UNREACHABLE_STREAK 3
// UPnP AVTransport control (GetPositionInfo — обложка альбома, см. arylic_metadata.cpp),
// отдельный от основного HTTPS-порта запрос. Порт и путь найдены live 2026-09-12 через
// description.xml на реальном устройстве (Linkplay.4.6.529755) — если Arylic заменят на
// другое физическое устройство/прошивку, пересмотреть тем же способом
#define ARYLIC_UPNP_PORT 49152
#define ARYLIC_UPNP_CONTROL_PATH "/upnp/control/rendertransport1"

// Weather (see weather.h) — server-side port of the web UI's own IP-geolocation + Open-Meteo
// polling (web_control.cpp's loadWeather() JS), so the knob (no browser, no IP of its own worth
// geolocating) gets the same data via buildStatusJson()'s weather fields / GET /weather.
// 30 minutes, same cadence the web UI's own setInterval(loadWeather,1800000) already uses —
// weather doesn't change fast enough to justify polling more often, and both calls below are
// free public APIs it'd be rude to hammer.
#define WEATHER_POLL_INTERVAL_MS 1800000UL
// Retry sooner than the full interval after a failure (e.g. right after boot, before DNS/Wi-Fi
// has fully settled, or a transient outage of either public API) — otherwise a single bad poll
// would leave the badge empty for up to 30 minutes.
#define WEATHER_POLL_RETRY_MS 120000UL

// --- Wi-Fi: подключение сохранённой сетью + реконнект (wifi_setup.h/.cpp) ---
// SSID/пароль хранятся в NVS (Preferences), не в коде — см. wifiSaveCredentials()/
// wifiForgetCredentials() и wifi_provisioning.h за тем, как они туда попадают.
//
// Сколько ждём подключения СОХРАНЁННОЙ сетью при старте, прежде чем считать её недоступной
// и перейти в режим настройки (wifi_provisioning.h) — типичный случай: устройство физически
// перенесли в другой дом/сеть, старая сеть больше не видна
#define WIFI_CONNECT_TIMEOUT_MS 15000UL
// После разрыва связи с УЖЕ подключённой сетью (не при первом старте) — переподключение с
// нарастающей паузой: первая попытка через WIFI_RECONNECT_BASE_MS, дальше пауза удваивается
// на каждой неудачной попытке вплоть до WIFI_RECONNECT_MAX_MS, и сбрасывается обратно к
// базовой сразу после успешного подключения (см. onWifiEvent() в wifi_setup.cpp)
#define WIFI_RECONNECT_BASE_MS 2000UL
#define WIFI_RECONNECT_MAX_MS 30000UL
// Раз в сколько мс повторно слать свой IP на Mega (megaLinkSendIp()), пока связь есть — не
// только при самом подключении. Нужно на случай, если Mega перезагрузится/пропустит момент
// первого IP (например включились не одновременно) — без этого она бы никогда не узнала
// адрес заново, раз ESP32 просто не переподключается лишний раз без причины
#define WIFI_IP_RESEND_INTERVAL_MS 60000UL

// --- Режим настройки Wi-Fi (wifi_provisioning.h/.cpp) — своя открытая точка доступа,
// поднимается, когда сохранённой сети нет или она недоступна за WIFI_CONNECT_TIMEOUT_MS ---
#define WIFI_PROVISION_AP_SSID "AudioCtrl-Setup"

// --- Wi-Fi handoff для KnobControl (ARCHITECTURE.md "Wi-Fi handoff") — служебная точка
// доступа, поднимается ОДНОВРЕМЕННО с обычным подключением к домашней сети (WIFI_AP_STA, не
// взамен него), с фиксированным (зашитым в оба репозитория — см. KnobControl's wifi_driver.cpp)
// SSID/паролем. Кноп цепляется сюда один раз при пэйринге/ресинхронизации, забирает домашние
// креды через GET /wifi-creds и переключается сам. НЕ может одновременно работать с
// WIFI_PROVISION_AP_SSID выше — у ESP32 только одна Wi-Fi-рация и один SoftAP SSID
// одновременно (см. ARCHITECTURE.md за тем, почему это осознанное ограничение, не баг)
#define WIFI_SERVICE_AP_SSID "PathfinderLink"
#define WIFI_SERVICE_AP_PASSWORD "pathfinder-link" // >=8 символов, требование WPA2
