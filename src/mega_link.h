#pragma once

// ============================================================================
// mega_link.h/.cpp — единственный канал в сторону Mega: аппаратный UART (Serial2),
// однонаправленный (ESP32 TX -> Mega RX2). Mega по этому проекту НИЧЕГО не отправляет
// назад (см. README.md, "Mega только слушает") — поэтому здесь только send*(), нет
// read/receive. Протокол — простые текстовые строки, см. описание в README.md,
// "Протокол UART".
//
// Приёмник на стороне Mega реализован (esp32_link.h/.cpp, ветка feature/esp32-uart-receiver
// в репозитории 260422-030547-megaatmega2560, 2026-09-10) — CMD:/IP:/PLAY:/ARYLIC:
// принимаются и используются (META:/PLAY: двигают полноэкранный "Now Playing" и
// автопереключение Source, IP:/ARYLIC: показываются в пункте меню Info). CMD: пока
// распознаётся, но не исполняется — см. esp32_link.h в том репозитории.
// ============================================================================

#include <Arduino.h>
#include <IPAddress.h>

void megaLinkBegin();

// Отправляет "CMD:<letter>\n" — letter один из: R L E U D M P S
// (right/left/enter/up/down/mute/power/set — тот же словарь действий, что был
// у WEB_ACTIONS в experiment/ethernet-arylic-webctl/src/web_control.cpp на Mega)
void megaLinkSendCommand(char actionLetter);

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
