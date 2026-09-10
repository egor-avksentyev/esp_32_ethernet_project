#pragma once

// ============================================================================
// config.h — все константы проекта в одном месте (по образцу hardware_settings.h
// в прошивке Mega): пины, протокол UART к Mega, адрес Arylic, тайминги.
// ============================================================================

#include <Arduino.h>

// --- UART к Mega (mega_link.h/.cpp) ---
// Используем аппаратный UART2 (Serial2 на ESP32) — UART0 занят USB/монитором и логом,
// UART1 на многих платах занят SPI flash. Линия одна: ESP32 TX -> Mega RX (см. README.md,
// "Mega только слушает" — обратного провода нет, RX2 этого ESP32 физически ни к чему
// не подключаем).
#define MEGA_LINK_TX_PIN 17
#define MEGA_LINK_RX_PIN 16 // не используется (Mega ничего не шлёт назад), но begin() требует пин
#define MEGA_LINK_BAUD 115200
#define MEGA_LINK_META_MAX_LEN 40 // Максимум символов в строке "META:" — под однострочный показ на OLED

// --- Веб-страница управления (web_control.h/.cpp) ---
#define WEB_SERVER_PORT 80

// --- Метадата Arylic (arylic_metadata.h/.cpp) ---
// Тот же статический IP, что был в ETHERNET_ARYLIC_WEBUI_PLAN.md / experiment/ethernet-arylic-webctl
// на Mega-репозитории — поправь, если роутер выдаст другой (DHCP-резервации на MAC Arylic нет)
#define ARYLIC_IP_OCTETS 192, 168, 1, 139
#define ARYLIC_POLL_INTERVAL_MS 3000

// --- Wi-Fi реконнект ---
#define WIFI_RECONNECT_CHECK_MS 5000
