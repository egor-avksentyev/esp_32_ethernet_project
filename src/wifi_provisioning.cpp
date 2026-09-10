#include "wifi_provisioning.h"
#include "wifi_setup.h"
#include "config.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

static WebServer server(80);
static DNSServer dnsServer;
static const byte DNS_PORT = 53;

static void handleRoot() {
  // Блокирует на пару секунд — приемлемо здесь: это разовая настройка, не горячий путь
  int n = WiFi.scanNetworks();
  String options;
  for (int i = 0; i < n; i++) {
    options += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) +
               " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }

  String page =
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Настройка Wi-Fi</title><style>"
    "body{font-family:sans-serif;background:#111;color:#eee;text-align:center;padding:24px}"
    "input,select,button{font-size:1.1em;margin:8px 0;padding:10px;width:90%;max-width:320px;"
    "border-radius:8px;border:none;box-sizing:border-box}"
    "button{background:#4a90d9;color:#fff}"
    "</style></head><body>"
    "<h2>Подключение к Wi-Fi</h2>"
    "<form action='/save' method='POST'>"
    "<select onchange=\"document.getElementById('ssid').value=this.value\">"
    "<option value=''>-- выбери сеть из списка --</option>" + options + "</select><br>"
    "<input type='text' id='ssid' name='ssid' placeholder='или впиши имя сети вручную' required><br>"
    "<input type='password' name='password' placeholder='пароль'><br>"
    "<button type='submit'>Подключить</button>"
    "</form></body></html>";

  server.send(200, "text/html", page);
}

static void handleSave() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  if (ssid.length() == 0) {
    server.send(400, "text/plain", "Не указана сеть");
    return;
  }
  wifiSaveCredentials(ssid.c_str(), password.c_str());
  server.send(200, "text/html",
    "<!DOCTYPE html><html><head><meta charset=utf-8></head>"
    "<body style='font-family:sans-serif;text-align:center;padding-top:40px;background:#111;color:#eee'>"
    "<h2>Сохранено</h2><p>Перезагружаюсь и подключаюсь к \"" + ssid + "\"...</p></body></html>");
  delay(1000); // даём TCP-ответу реально уйти клиенту, прежде чем рвать сеть перезагрузкой
  ESP.restart();
}

void wifiProvisioningBegin() {
  // AP_STA, не чистый AP — WiFi.scanNetworks() в handleRoot() требует активный STA-радио,
  // даже если он ни к чему не подключается в этом режиме
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(WIFI_PROVISION_AP_SSID);
  IPAddress apIp = WiFi.softAPIP();
  Serial.print("[wifi-setup] точка доступа поднята: ");
  Serial.print(WIFI_PROVISION_AP_SSID);
  Serial.print(", зайди на http://");
  Serial.println(apIp);

  dnsServer.start(DNS_PORT, "*", apIp);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleRoot); // см. wifi_provisioning.h — часть captive-portal трюка
  server.begin();
}

void wifiProvisioningPoll() {
  dnsServer.processNextRequest();
  server.handleClient();
}
