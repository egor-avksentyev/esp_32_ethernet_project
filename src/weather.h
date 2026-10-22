#pragma once

// ============================================================================
// weather.h/.cpp — background poll for current weather at this device's own location.
//
// Server-side port of the web UI's existing client-side JS (web_control.cpp's loadWeather(),
// WEATHER_LABEL_BY_CODE/WEATHER_ICONS) — same two free, keyless, CORS-open public APIs:
//   1. IP geolocation — https://ipwho.is/ first, http://ip-api.com/json/ as a fallback — gives
//      lat/lon/city for "wherever this device's own internet connection is", which is more
//      reliable than the web page's version (that geolocates whoever's BROWSER opened the page,
//      wrong if opened remotely, not from home).
//   2. Current conditions — https://api.open-meteo.com/v1/forecast?...&current_weather=true —
//      gives a WMO weathercode + temperature for that lat/lon. Current conditions only, no
//      forecast (the web UI doesn't show one either, nothing to mirror).
//
// Exists so KnobControl (no browser, nothing of its own to geolocate) can show the same data —
// see ARCHITECTURE.md "Статус-бейджи на Home — Wi-Fi, заряд батареи, погода" in that repo.
// Delivered to it piggybacked on the existing buildStatusJson()/WS82 status push (web_control.cpp)
// rather than a dedicated poll from the knob — that channel already updates every
// LIVE_WS_PUSH_INTERVAL_MS regardless, reading a mutex-protected cache here costs nothing extra.
// GET /weather below exists too, for parity with the web UI and direct curl/debugging use.
// ============================================================================

#include <Arduino.h>

// Starts the background poll task (own FreeRTOS task, core 0, same pattern as
// arylic_metadata.h's arylicMetadataBegin()) — call once from setup(), after Wi-Fi is up.
void weatherBegin();

// true once the first successful poll has completed — city/tempC/wmoCode below are meaningless
// before that (never guessed/defaulted to something plausible-looking).
bool weatherKnown();

String weatherCity();  // as returned by the geolocation service, not translated (proper noun)
int weatherTempC();    // rounded, like the web UI's own Math.round()
int weatherWmoCode();  // raw Open-Meteo WMO code — caller groups it into a label/icon family
