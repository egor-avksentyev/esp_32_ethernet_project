#pragma once

// ============================================================================
// arylic_metadata.h/.cpp — периодический опрос локального HTTP API Arylic-стримера
// (WiFi должен быть уже поднят). Портировано из experiment/ethernet-arylic-webctl
// (Mega-репозиторий, src/arylic_metadata.cpp) на HTTPClient/WiFiClient вместо
// EthernetClient — сама логика (диагностический каркас, не полный парсинг) не менялась.
//
// ТО ЖЕ известное ограничение, что было на Mega-ветке: точный формат ответа
// (JSON/hex, имена полей) не подтверждён на реальном устройстве — см.
// ETHERNET_ARYLIC_WEBUI_PLAN.md в Mega-репозитории, открытые вопросы. Сейчас
// pollArylicMetadata() печатает сырой ответ в Serial, чтобы увидеть реальный формат
// перед тем, как писать разбор и пересылку title/artist на Mega через
// megaLinkSendMetadata().
// ============================================================================

#include <Arduino.h>

void pollArylicMetadata();
