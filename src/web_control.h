#pragma once

// ============================================================================
// web_control.h/.cpp — простая веб-страница управления (телефон/браузер в локальной
// сети), портирована из experiment/ethernet-arylic-webctl (Mega-репозиторий,
// src/web_control.cpp). Разница с той версией: там кнопки напрямую дёргали
// executeMenuCommand() (сервер жил на той же Mega, что и вся логика меню) — здесь
// ESP32 не знает о меню вообще, кнопка лишь шлёт однобуквенную команду на Mega через
// megaLinkSendCommand() (mega_link.h). Сама HTML-страница и словарь действий
// (right/left/enter/up/down/mute/power/set) — тот же.
//
// Использует штатный WebServer.h (arduino-esp32) вместо ручного разбора сокета,
// как было на W5500/EthernetServer — на ESP32 в этом нет нужды, библиотека уже
// неблокирующая на уровне handleClient().
// ============================================================================

#include <Arduino.h>

void webControlBegin();
void webControlPoll(); // вызывать из loop() каждую итерацию

// Гасит/зажигает веб-страницу и ставит/снимает паузу через локальный Arylic API — общая точка
// для кнопки "Power" на самой странице (handleCmd()) и настоящего сигнала POWER: от Mega по
// UART (см. mega_link.cpp). off — новое состояние (true = выключено); вызов с уже текущим
// значением ничего не делает (см. определение в web_control.cpp)
void applyWebPowerState(bool off);

// Рассылает сырой бинарный кадр (128x64/8 = 1024 байта, tile-формат u8g2) всем клиентам,
// подключённым к тому же live-WebSocket (LIVE_WS_PORT, config.h), что уже используется для
// push статуса/трека — см. frame_mirror.h/.cpp за источником данных. Ничего не делает, если
// ни один клиент не подключён (см. определение в web_control.cpp)
void webControlBroadcastFrame(const uint8_t* buf, size_t len);
