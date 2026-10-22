#include "weather.h"
#include "config.h"
#include "wifi_setup.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <cmath>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// Same mutex-guards-a-few-plain-fields pattern as arylic_metadata.cpp: one background task
// writes, HTTP handlers (main thread) read, critical sections are just assignments — the actual
// network calls happen entirely outside the lock.
static SemaphoreHandle_t stateMutex = nullptr;

struct MutexGuard {
  SemaphoreHandle_t handle;
  explicit MutexGuard(SemaphoreHandle_t h) : handle(h) { xSemaphoreTake(handle, portMAX_DELAY); }
  ~MutexGuard() { xSemaphoreGive(handle); }
};

static bool knownFlag = false;
static char cityBuf[48] = "";
static int tempCVal = 0;
static int wmoCodeVal = 0;

bool weatherKnown() { MutexGuard g(stateMutex); return knownFlag; }
String weatherCity() { MutexGuard g(stateMutex); return String(cityBuf); }
int weatherTempC() { MutexGuard g(stateMutex); return tempCVal; }
int weatherWmoCode() { MutexGuard g(stateMutex); return wmoCodeVal; }

namespace {

// Same manual-substring-search style as arylic_metadata.cpp's extractLongField()/
// extractStringField() — no ArduinoJson dependency anywhere else in this codebase, and every
// field used below has a key name that's unique within its own response, so a flat search works
// even though open-meteo's temperature/weathercode are technically nested one level
// ("current_weather":{...}) — nothing else in that payload reuses those key names.
double extractDoubleField(const String &payload, const char *key) {
  int idx = payload.indexOf(key);
  if (idx < 0) return 0;
  return atof(payload.c_str() + idx + strlen(key));
}

void extractStringField(const String &payload, const char *key, char *out, size_t outMax) {
  out[0] = '\0';
  int idx = payload.indexOf(key);
  if (idx < 0) return;
  const char *start = payload.c_str() + idx + strlen(key);
  const char *end = strchr(start, '"');
  if (!end) return;
  size_t len = end - start;
  if (len >= outMax) len = outMax - 1;
  memcpy(out, start, len);
  out[len] = '\0';
}

// GET over HTTPS with a short handshake timeout — same reasoning as arylic_metadata.cpp's
// sendArylicCommand(): WiFiClientSecure's default 120s handshake timeout would otherwise be able
// to stall this whole background task for 2 minutes on a bad TLS attempt. Shorter than arylic's
// own 8s there, though — that value was tuned for one specific flaky local device; ipwho.is/
// open-meteo are reliable public cloud APIs with no reason to need that much slack, and this
// task now runs on core 1 (see weatherBegin()) alongside loop()'s own UART polling, so keeping
// the worst-case stall short matters more here than it did for arylic's dedicated core 0 task.
bool httpsGet(const String &url, String &body) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(4);
  HTTPClient http;
  http.setTimeout(3000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  bool ok = code == HTTP_CODE_OK;
  if (ok) body = http.getString();
  http.end();
  client.stop(); // see arylic_metadata.cpp's own comment: http.end() skips client->stop() when
                  // the handshake itself failed, leaking mbedTLS buffers if we don't do it here
  return ok;
}

bool httpGetPlain(const String &url, String &body) {
  HTTPClient http;
  http.setTimeout(3000);
  if (!http.begin(url)) return false;
  int code = http.GET();
  bool ok = code == HTTP_CODE_OK;
  if (ok) body = http.getString();
  http.end();
  return ok;
}

// Mirrors web_control.cpp's fetchLocation(): ipwho.is first, ip-api.com/json as a fallback.
bool fetchLocation(double &lat, double &lon, char *cityOut, size_t cityOutMax) {
  String body;
  if (httpsGet("https://ipwho.is/", body) && body.indexOf("\"success\":true") >= 0) {
    lat = extractDoubleField(body, "\"latitude\":");
    lon = extractDoubleField(body, "\"longitude\":");
    extractStringField(body, "\"city\":\"", cityOut, cityOutMax);
    if (lat != 0 || lon != 0) return true;
  }
  body = "";
  if (httpGetPlain("http://ip-api.com/json/", body) &&
      body.indexOf("\"status\":\"success\"") >= 0) {
    lat = extractDoubleField(body, "\"lat\":");
    lon = extractDoubleField(body, "\"lon\":");
    extractStringField(body, "\"city\":\"", cityOut, cityOutMax);
    return true;
  }
  return false;
}

bool fetchCurrentWeather(double lat, double lon, int &tempCOut, int &wmoCodeOut) {
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 4) +
               "&longitude=" + String(lon, 4) + "&current_weather=true";
  String body;
  if (!httpsGet(url, body)) return false;
  // Real bug, found by curling the live API directly (not just read as plausible from memory):
  // the response ALWAYS includes a "current_weather_units" object BEFORE "current_weather" —
  // {"current_weather_units":{"temperature":"\xC2\xB0""C",...,"weathercode":"wmo code"},
  //  "current_weather":{"temperature":11.5,...,"weathercode":3}}. extractDoubleField()'s naive
  // first-match indexOf() was finding "temperature"/"weathercode" inside THAT units object
  // instead — their values there are QUOTED STRINGS ("°C"/"wmo code"), and atof() on a string
  // starting with a quote character returns 0.0. Every poll "succeeded" (current_weather was
  // present) but silently produced 0°/code 0 — exactly the "показывает 0 градусов" symptom
  // reported live. Fix: locate "current_weather":{ itself and search only from there on.
  int cwIdx = body.indexOf("\"current_weather\":{");
  if (cwIdx < 0) return false;
  String cw = body.substring(cwIdx);
  tempCOut = (int)std::lround(extractDoubleField(cw, "\"temperature\":"));
  wmoCodeOut = (int)extractDoubleField(cw, "\"weathercode\":");
  return true;
}

bool weatherPollOnce() {
  double lat = 0, lon = 0;
  char city[48];
  if (!fetchLocation(lat, lon, city, sizeof(city))) {
    Serial.println("[weather] geolocation failed (both ipwho.is and ip-api.com)");
    return false;
  }
  int tempC, wmoCode;
  if (!fetchCurrentWeather(lat, lon, tempC, wmoCode)) {
    Serial.println("[weather] open-meteo request failed");
    return false;
  }
  {
    MutexGuard g(stateMutex);
    strncpy(cityBuf, city, sizeof(cityBuf) - 1);
    cityBuf[sizeof(cityBuf) - 1] = '\0';
    tempCVal = tempC;
    wmoCodeVal = wmoCode;
    knownFlag = true;
  }
  Serial.printf("[weather] %s: %d\xC2\xB0""C, code %d\n", city, tempC, wmoCode);
  return true;
}

void weatherPollTask(void * /*param*/) {
  for (;;) {
    bool ok = false;
    if (wifiIsConnected()) {
      ok = weatherPollOnce();
    }
    vTaskDelay(pdMS_TO_TICKS(ok ? WEATHER_POLL_INTERVAL_MS : WEATHER_POLL_RETRY_MS));
  }
}

} // namespace

void weatherBegin() {
  if (stateMutex == nullptr) {
    stateMutex = xSemaphoreCreateMutex();
  }
  // Real crash, found live (2026-10-23): core 0 already hosts arylic_poll (its own TLS-heavy
  // HTTPS calls every ARYLIC_POLL_INTERVAL_MS=500ms) plus the WS/HTTP server tasks
  // (web_control.cpp's live_ws/web_http). Putting this task's own TLS handshakes (up to ~8s
  // each, twice per poll) on the SAME core tipped total core-0 load over the edge — the idle
  // task stopped getting scheduled long enough to feed the Task Watchdog, and the whole board
  // panic-rebooted (`task_wdt: ... IDLE0 (CPU 0)`), repeatedly, since this task's very first
  // poll fires immediately at boot and the crash landed right back in the same collision every
  // time. Core 1 only runs loop()'s own lightweight polling (UART/motor_ws/frame_mirror, all
  // just buffer reads) — plenty of room for an HTTPS round-trip that only happens twice an hour.
  // Stack size (12288) still matches arylic_metadata.cpp's own TLS task — same mbedTLS stack
  // pressure applies regardless of which core runs it.
  xTaskCreatePinnedToCore(weatherPollTask, "weather_poll", 12288, nullptr, 1, nullptr, 1);
}
