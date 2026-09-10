#pragma once

// ============================================================================
// mega_link.h/.cpp — единственный канал в сторону Mega: аппаратный UART (Serial2),
// однонаправленный (ESP32 TX -> Mega RX2). Mega по этому проекту НИЧЕГО не отправляет
// назад (см. README.md, "Mega только слушает") — поэтому здесь только send*(), нет
// read/receive. Протокол — простые текстовые строки, см. описание в README.md,
// "Протокол UART".
//
// ВАЖНО: приёмная часть на самой Mega пока НЕ реализована (сознательно отложено,
// см. README.md) — этот модуль просто шлёт байты в UART; пока Mega их не слушает,
// физически ничего не произойдёт.
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
