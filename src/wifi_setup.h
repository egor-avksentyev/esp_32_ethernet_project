#pragma once

// ============================================================================
// wifi_setup.h/.cpp — подключение к Wi-Fi. Сейчас — зашитые SSID/пароль
// (wifi_credentials.h, не в git), способ ввода при переезде между точками доступа
// (captive portal и т.п.) осознанно отложен, см. README.md, "Wi-Fi credentials".
// ============================================================================

#include <Arduino.h>

void wifiSetupBegin();

// Вызывать из loop() — не блокирует; сама решает, пора ли проверять/переподключаться
// (не чаще WIFI_RECONNECT_CHECK_MS, config.h)
void wifiSetupMaintain();

bool wifiIsConnected();
