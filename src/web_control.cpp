#include "web_control.h"
#include "config.h"
#include "mega_link.h"
#include "wifi_setup.h"
#include "arylic_metadata.h"
#include <WebServer.h>

static WebServer server(WEB_SERVER_PORT);

// Последняя команда, отправленная на Mega — единственное, что ESP32 реально "знает"
// о состоянии системы (Mega ничего не отправляет назад, см. mega_link.h). /status
// показывает это плюс собственное состояние ESP32 (Wi-Fi), НЕ настоящее меню/громкость/
// mute Mega — см. README.md, "Известное ограничение: статус на веб-странице"
static char lastActionSent = '\0';

struct WebAction {
  const char* name;
  char letter;
};
static const WebAction WEB_ACTIONS[] = {
  {"right", 'R'}, {"left", 'L'}, {"enter", 'E'}, {"mute", 'M'},
  {"power", 'P'}, {"up", 'U'}, {"down", 'D'}, {"set", 'S'},
};
static const uint8_t WEB_ACTIONS_COUNT = sizeof(WEB_ACTIONS) / sizeof(WEB_ACTIONS[0]);

// Держим страницу маленькой и в PROGMEM — тот же HTML/CSS/JS, что был в
// experiment/ethernet-arylic-webctl, только /status теперь текстовый статус ESP32,
// не меню Mega (см. комментарий у lastActionSent выше)
static const char PAGE_HTML[] PROGMEM =
  "<!DOCTYPE html><html><head><meta charset=utf-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>Control</title><style>"
  "body{font-family:sans-serif;text-align:center;background:#111;color:#eee}"
  "button{font-size:1.3em;margin:6px;padding:14px 22px;border-radius:8px;border:none;background:#333;color:#eee}"
  "button:active{background:#555}"
  "#status{margin:12px;font-size:1.1em;color:#8cf}"
  "</style></head><body>"
  "<h2>Bass/High/Volume</h2>"
  "<div id=status>...</div>"
  "<div><button onclick=cmd('left')>&larr;</button>"
  "<button onclick=cmd('enter')>OK</button>"
  "<button onclick=cmd('right')>&rarr;</button></div>"
  "<div>"
  "<button onmousedown=startHold('up') onmouseup=stopHold() onmouseleave=stopHold()"
  " ontouchstart=startHold('up') ontouchend=stopHold()>&uarr;</button>"
  "<button onmousedown=startHold('down') onmouseup=stopHold() onmouseleave=stopHold()"
  " ontouchstart=startHold('down') ontouchend=stopHold()>&darr;</button>"
  "</div>"
  "<div><button onclick=cmd('mute')>Mute</button>"
  "<button onclick=cmd('set')>Source</button>"
  "<button onclick=cmd('power')>Power</button></div>"
  "<div style='margin-top:14px'>"
  "<input id=arylicIp type=text placeholder='IP Arylic вручную' style='padding:8px;border-radius:6px;border:none'>"
  "<button id=arylicApply onclick=applyArylicIp()>Применить</button></div>"
  "<div><button onclick=forgetWifi() style='background:#733'>Сменить Wi-Fi</button></div>"
  "<script>"
  "function cmd(a){fetch('/cmd?action='+a)}"
  "let holdTimer=null;"
  "function startHold(a){cmd(a);holdTimer=setInterval(()=>cmd(a),150)}"
  "function stopHold(){if(holdTimer){clearInterval(holdTimer);holdTimer=null}}"
  "function poll(){fetch('/status').then(r=>r.text()).then(t=>{"
  "document.getElementById('status').innerText=t})}"
  "setInterval(poll,1500);poll();"
  "function applyArylicIp(){"
  "let v=document.getElementById('arylicIp').value;"
  "fetch('/arylic-ip?ip='+encodeURIComponent(v),{method:'POST'})"
  ".then(r=>{if(!r.ok)alert('Некорректный IP')})}"
  // Пока Arylic реально виден (см. arylicIsReachable() в arylic_metadata.cpp) — поле и кнопка
  // неактивны, ручной ввод не нужен; текущий определённый адрес подставляется в поле для
  // наглядности. Как только связь пропадает — поле включается само, без перезагрузки страницы
  "function pollArylic(){fetch('/arylic-status').then(r=>r.text()).then(t=>{"
  "let parts=t.split(' ');let ok=(parts[0]=='OK');let ip=parts[1]||'';"
  "let el=document.getElementById('arylicIp');"
  "el.disabled=ok;document.getElementById('arylicApply').disabled=ok;"
  "if(ok&&ip)el.value=ip})}"
  "setInterval(pollArylic,3000);pollArylic();"
  "function forgetWifi(){if(confirm('Забыть текущую Wi-Fi сеть и перезагрузиться в режим "
  "настройки?')){fetch('/wifi-forget',{method:'POST'})"
  ".then(()=>alert('Готово. Устройство подняло точку доступа " WIFI_PROVISION_AP_SSID "'))}}"
  "</script></body></html>";

static void handleRoot() {
  server.send_P(200, "text/html", PAGE_HTML);
}

static void handleCmd() {
  if (server.hasArg("action")) {
    String action = server.arg("action");
    for (uint8_t i = 0; i < WEB_ACTIONS_COUNT; i++) {
      if (action == WEB_ACTIONS[i].name) {
        megaLinkSendCommand(WEB_ACTIONS[i].letter);
        lastActionSent = WEB_ACTIONS[i].letter;
        break;
      }
    }
  }
  server.send(204);
}

static void handleStatus() {
  String status = wifiIsConnected() ? "Wi-Fi OK" : "Wi-Fi отключён";
  if (lastActionSent != '\0') {
    status += " | последняя команда: ";
    status += lastActionSent;
  }
  server.send(200, "text/plain", status);
}

static void handleNotFound() {
  handleRoot();
}

// "OK <ip>" или "FAIL <ip>" (<ip> может быть пустым, если ещё ни разу не определился) —
// используется JS на странице, чтобы включать/выключать поле ручного ввода и подставлять
// туда текущий адрес (см. PAGE_HTML, pollArylic())
static void handleArylicStatus() {
  String resp = arylicIsReachable() ? "OK " : "FAIL ";
  resp += arylicCurrentIp();
  server.send(200, "text/plain", resp);
}

// Ручной ввод IP Arylic с веб-страницы — пустая строка снимает override (см.
// setArylicIpOverride() в arylic_metadata.cpp за подробностями приоритета над mDNS)
static void handleArylicIp() {
  if (!server.hasArg("ip")) {
    server.send(400, "text/plain", "no ip");
    return;
  }
  String ip = server.arg("ip");
  IPAddress parsed;
  if (ip.length() > 0 && !parsed.fromString(ip)) {
    server.send(400, "text/plain", "invalid IP");
    return;
  }
  setArylicIpOverride(ip.c_str());
  server.send(200, "text/plain", "OK");
}

// Сознательная смена сети без физического переезда (см. wifi_provisioning.h за тем, зачем
// это нужно отдельно от автоматического ухода в настройку) — стирает сохранённые SSID/пароль
// и перезагружается; следующий wifiSetupBegin() (main.cpp) не найдёт сохранённой сети и сам
// поднимет AP-режим настройки
static void handleWifiForget() {
  wifiForgetCredentials();
  server.send(200, "text/plain", "OK, перезагружаюсь в режим настройки");
  delay(1000); // даём TCP-ответу уйти клиенту, прежде чем рвать сеть перезагрузкой
  ESP.restart();
}

void webControlBegin() {
  server.on("/", handleRoot);
  server.on("/cmd", handleCmd);
  server.on("/status", handleStatus);
  server.on("/wifi-forget", HTTP_POST, handleWifiForget);
  server.on("/arylic-status", handleArylicStatus);
  server.on("/arylic-ip", HTTP_POST, handleArylicIp);
  server.onNotFound(handleNotFound);
  server.begin();
}

void webControlPoll() {
  server.handleClient();
}
