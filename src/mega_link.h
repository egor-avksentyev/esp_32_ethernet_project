#pragma once

// ============================================================================
// mega_link.h/.cpp — канал связи с Mega: аппаратный UART (Serial2). До 2026-09-21 был
// однонаправленным (ESP32 TX -> Mega RX2, "Mega только слушает", см. README.md) — теперь
// двусторонний: Mega тоже шлёт (см. megaLinkPoll()/POWER:/TEMP:/VOLT: ниже), через level
// shifter (Mega — 5V логика, ESP32 GPIO не 5V-толерантны), MEGA_LINK_RX_PIN (config.h) с этого
// момента реально подключён и читается. Протокол — простые текстовые строки, см. описание в
// README.md, "Протокол UART", и в esp32_link.h Mega-репозитория (единый источник истины по
// формату сообщений в обе стороны).
//
// Приёмник на стороне Mega реализован (esp32_link.h/.cpp в репозитории
// 260422-030547-megaatmega2560) — CMD:/IP:/PLAY:/ARYLIC:/SRC:/POS: принимаются и используются
// (META:/PLAY:/SRC:/POS: двигают полноэкранный "Now Playing" и автопереключение Source, IP:/
// ARYLIC: показываются в пункте меню Info).
//
// KnobControl Фаза 4 (2026-10-20) — четыре новые входящие телеграммы от Mega: MENU:/
// SETTINGS:/MUTE:/STREAMER: (полный формат — esp32_link.h Mega-репозитория). ESP32 здесь
// играет роль ЧИСТОГО ПРОКСИ, без своей логики — handleMegaLine() просто перепаковывает
// остаток строки после префикса в "{"type":"<menu|settings|mute|streamer>","text":"..."}"
// и шлёт дальше по WS82 (см. webControlBroadcastText() в web_control.h) — тем же принципом,
// что уже есть у "arylic" (raw text field, не структурные поля; сам ESP32 не разбирает
// содержимое и не хранит его типизированно, в отличие от POWER:/TEMP:/VOLT: выше). Знает
// значение каждого поля только кноп (другой репозиторий), ему и положено. В обратную
// сторону — megaLinkSendJump() ниже, зеркально CMD:J<index> от кнопа.
// ============================================================================

#include <Arduino.h>
#include <IPAddress.h>

void megaLinkBegin();

// Вызывать из loop() каждую итерацию — разбирает POWER:/TEMP:/VOLT: от Mega (см. ниже),
// тем же построчным приёмом, что esp32LinkPoll() на стороне Mega. При получении POWER: сама
// вызывает applyWebPowerState() (web_control.h) — веб-страница гаснет/паузит Spotify
// синхронно с реальным пультом, не только со своей собственной кнопки Power
void megaLinkPoll();

// true, если Mega хоть раз прислала своё состояние питания (POWER:) — до первого сообщения
// (например сразу после перезагрузки ESP32) считать неизвестным, не "включено" по умолчанию
bool megaLinkPowerKnown();
bool megaLinkIsPoweredOn();

// Температуры трёх ламп (см. TEMP: в esp32_link.h Mega-репозитория) — TEMP_SENSOR_INVALID
// там же (-127.0), если конкретный датчик не отвечает. Валидно смотреть только после того,
// как пришло хотя бы одно сообщение TEMP: (изначально все три — тот же сентинел -127.0)
float megaLinkTemp(uint8_t index);

// Напряжение сети (целые вольты) — см. VOLT: выше. megaLinkVoltageKnown() — то же самое
// различение "ещё не приходило"/"пришло 0", что у megaLinkPowerKnown()
bool megaLinkVoltageKnown();
int megaLinkVoltage();

// Отправляет "CMD:<letter>\n" — letter один из: R L E U D M P S X
// (right/left/enter/up/down/mute/power/set/exit — тот же словарь действий, что был
// у WEB_ACTIONS в experiment/ethernet-arylic-webctl/src/web_control.cpp на Mega; X — новое,
// KnobControl Фаза 4, см. esp32_link.h Mega-репозитория)
void megaLinkSendCommand(char actionLetter);

// Отправляет "CMD:J<index>\n" (KnobControl Фаза 4) — прыжок сразу на пункт меню <index>
// (0..MENU_ITEM_COUNT-1 на стороне Mega, порядок как в её menuItems[]) и вход в его
// настройку одним действием. ESP32 не знает, что означает конкретное число — чистый проброс
// того, что пришло от кнопа (см. esp32_link.h Mega-репозитория за полным описанием)
void megaLinkSendJump(int index);

// Отправляет "META:<text>\n" — text без переводов строк, обрезается до разумной длины
// под однострочный показ на OLED (см. MEGA_LINK_META_MAX_LEN в mega_link.cpp)
void megaLinkSendMetadata(const char* text);

// Отправляет "IP:<a.b.c.d>\n" — текущий IP ESP32 в РЕАЛЬНОЙ сети (не 192.168.4.1 из режима
// настройки, см. wifi_provisioning.h — эта функция не вызывается, пока идёт провижининг).
// Вызывается из wifi_setup.cpp при получении IP и затем периодически (см. config.h) — так
// Mega (когда появится приёмник, см. README.md) сможет показать актуальный адрес на своём
// экране, не заставляя лезть в Serial-монитор или полагаться на mDNS с телефона
void megaLinkSendIp(const IPAddress& ip);

// Отправляет "PLAY:1\n" или "PLAY:0\n" — играет ли Arylic ПРЯМО СЕЙЧАС (status=="play" в
// его API). На стороне Mega это (не присутствие META:) двигает автопереключение Source на
// Streamer и полноэкранный "Now Playing" — отправляется на каждом опросе
// (pollArylicMetadata(), arylic_metadata.cpp), не только по факту смены состояния, чтобы Mega
// не застряла в устаревшем состоянии, если пропустила один кадр
void megaLinkSendPlayState(bool playing);

// Отправляет "ARYLIC:OK\n" или "ARYLIC:FAIL\n" — смог ли ESP32 достучаться до Arylic по
// сети на этом опросе (не то же самое, что playing — устройство может быть доступно, но
// ничего не играть). Для показа в пункте меню Info на Mega ("Arylic ok"/"disconnected")
void megaLinkSendArylicStatus(bool ok);

// Отправляет "SRC:<name>\n" — источник воспроизведения ("Spotify", "AirPlay", ...; см.
// computeSourceName() в arylic_metadata.cpp), не путать с пунктом меню "Source" на Mega
// (тот про физическое реле AUX/CD/DAT/Streamer, это — про то, какой стриминг-сервис/
// протокол сейчас играет через Arylic). Отправляется на каждом опросе, пока играет — тем
// же принципом, что и PLAY:/META: выше. name может быть пустой строкой (источник не
// распознан) — text без переводов строк, обрезается до разумной длины
void megaLinkSendSource(const char* name);

// Отправляет "POS:<posMs>:<lenMs>\n" — позиция и общая длительность трека НА МОМЕНТ этого
// опроса (см. trackPosMs/trackLenMs в arylic_metadata.cpp — те же значения, что уходят на
// веб-страницу через /track). Mega сама досчитывает позицию между кадрами по своему millis()
// (как и веб-страница по Date.now(), см. arylicTrackAgeMs() в arylic_metadata.h) — не нужно
// слать эту команду чаще, чем раз в опрос. Не отправляется, пока не играет (см. вызов в
// arylic_metadata.cpp) — Mega просто не обновляет прогресс-бар, пока не пришло PLAY:1
void megaLinkSendPosition(long posMs, long lenMs);
