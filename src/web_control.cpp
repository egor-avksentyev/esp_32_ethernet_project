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
  "<div style='font-size:.85em;color:#aaa;margin-top:6px'>"
  "<span id=dtClock>--:--:--</span> &middot; <span id=dtDate></span>"
  "<div id=weatherInfo style='margin-top:2px;color:#8cf'></div></div>"
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
  "<div id=trackWrap style='margin-top:14px;display:none'>"
  "<img id=trackArt style='display:none;max-width:120px;border-radius:6px;margin-bottom:6px'>"
  "<div id=trackSource style='font-size:.8em;color:#8cf;display:none'></div>"
  "<div id=trackTitle style='font-size:.95em;color:#ccc;margin-bottom:4px'></div>"
  "<div id=trackProgress>"
  "<div style='background:#333;border-radius:6px;height:8px;overflow:hidden'>"
  "<div id=trackBar style='background:#8cf;height:100%;width:0%'></div></div>"
  "<div style='font-size:.8em;color:#888;margin-top:2px'>"
  "<span id=trackCur>0:00</span> / <span id=trackLen>0:00</span></div>"
  "</div></div>"
  "<div id=playbackWrap style='margin-top:10px;display:none'>"
  "<button onclick=playerCmd('prev')>&laquo;</button>"
  "<button onclick=playerCmd('onepause')>Play/Pause</button>"
  "<button onclick=playerCmd('next')>&raquo;</button>"
  "<div style='margin-top:8px'>"
  "<input id=volSlider type=range min=0 max=100 value=50 style='width:70%' "
  "oninput='volDragging=true' onchange=setVolume(this.value)>"
  "</div></div>"
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
  "if(ok&&ip)el.value=ip;"
  // Play/Pause/Next/Prev и громкость видны, только пока Arylic реально доступен — не привязано
  // к j.playing (трек может быть на паузе, громкость и play всё равно нужны)
  "document.getElementById('playbackWrap').style.display=ok?'block':'none'"
  "})}"
  "setInterval(pollArylic,3000);pollArylic();"
  // play/pause через "onepause" — сам переключает состояние, не полагаясь на то, что
  // getPlayerStatus считает текущим (это поле неточное для AirPlay, см. arylic_metadata.h)
  "function playerCmd(a){fetch('/playback?action='+a)}"
  // volDragging блокирует перезапись ползунка живым опросом, пока палец/курсор ещё на нём —
  // без этого ползунок дёргался бы обратно к старому значению между отпусканием и applied-ответом
  "let volDragging=false;"
  "function setVolume(v){fetch('/volume?value='+v);setTimeout(()=>{volDragging=false},1000)}"
  // Прогресс трека: /track опрашивается раз в 3с (как всё остальное), а между опросами
  // позиция досчитывается локально по реальному прошедшему времени (Date.now()), чтобы
  // полоска ехала плавно, а не прыгала раз в 3с — см. arylic_metadata.h за объяснением age
  "let trackPos=0,trackLen=0,trackFetchTime=0,trackPlayingNow=false;"
  "function fmtTime(ms){let s=Math.max(0,Math.floor(ms/1000));let m=Math.floor(s/60);s=s%60;"
  "return m+':'+(s<10?'0':'')+s}"
  "function pollTrack(){fetch('/track').then(r=>r.json()).then(j=>{"
  "trackPlayingNow=j.playing;trackLen=j.len;trackPos=j.pos+j.age;trackFetchTime=Date.now();"
  "document.getElementById('trackWrap').style.display=j.playing?'block':'none';"
  "if(j.playing)document.getElementById('trackTitle').innerText=j.text;"
  // Источник (Spotify/AirPlay/...) — отдельная строка, показывается независимо от того,
  // распарсились ли title/artist (AirPlay их не отдаёт вообще, но источник знать можно)
  "let src=document.getElementById('trackSource');"
  "if(j.playing&&j.source){src.innerText=j.source;src.style.display='block'}"
  "else{src.style.display='none'}"
  // Позиция трека для AirPlay не двигается вообще (устройство её не отдаёт ни в одном
  // известном API, проверено live — см. arylic_metadata.h) — полоска бы просто застыла на
  // месте и вводила в заблуждение, поэтому для этого источника прячем её целиком
  "document.getElementById('trackProgress').style.display=(j.source==='AirPlay')?'none':'block';"
  // Обложка отдаётся ссылкой на CDN сервиса-источника, не байтами — сам img её и грузит.
  // Пусто, если сервис её не отдаёт (например AirPlay/Apple Music, см. arylic_metadata.h)
  "let art=document.getElementById('trackArt');"
  "if(j.playing&&j.art){if(art.src!==j.art)art.src=j.art;art.style.display='block'}"
  "else{art.style.display='none'}"
  "if(!volDragging&&j.vol>=0)document.getElementById('volSlider').value=j.vol})}"
  "function tickTrack(){if(!trackPlayingNow)return;"
  "let pos=trackPos+(Date.now()-trackFetchTime);if(trackLen>0&&pos>trackLen)pos=trackLen;"
  "document.getElementById('trackCur').innerText=fmtTime(pos);"
  "document.getElementById('trackLen').innerText=fmtTime(trackLen);"
  "document.getElementById('trackBar').style.width=(trackLen>0?(100*pos/trackLen):0)+'%'}"
  "setInterval(pollTrack,3000);pollTrack();setInterval(tickTrack,500);"
  // Дата/время — часы самого браузера, без сети (страница отдаётся по обычному HTTP, а
  // navigator.geolocation в незащищённом контексте браузеры всё равно не дают использовать —
  // поэтому геолокация ниже не через GPS, а по IP через сторонние публичные API)
  "function pad(n){return n<10?'0'+n:n}"
  "function tickClock(){let d=new Date();"
  "document.getElementById('dtClock').innerText=pad(d.getHours())+':'+pad(d.getMinutes())+':'+pad(d.getSeconds());"
  "document.getElementById('dtDate').innerText=d.toLocaleDateString()}"
  "setInterval(tickClock,1000);tickClock();"
  // Погода — по геолокации через IP (не GPS): ipwho.is первым, ip-api.com — фолбэк, если
  // первый недоступен/перегружен (оба публичные, бесплатные, без ключа, с CORS *). Погода —
  // через Open-Meteo (тоже бесплатный, без ключа, CORS *), коды по таблице WMO
  "const WEATHER_CODES={0:'Ясно',1:'Ясно',2:'Переменная облачность',3:'Пасмурно',"
  "45:'Туман',48:'Туман',51:'Морось',53:'Морось',55:'Морось',56:'Ледяная морось',"
  "57:'Ледяная морось',61:'Дождь',63:'Дождь',65:'Сильный дождь',66:'Ледяной дождь',"
  "67:'Ледяной дождь',71:'Снег',73:'Снег',75:'Сильный снег',77:'Снежная крупа',"
  "80:'Ливень',81:'Ливень',82:'Сильный ливень',85:'Снегопад',86:'Снегопад',"
  "95:'Гроза',96:'Гроза с градом',99:'Гроза с градом'};"
  "function fetchLocation(){"
  "return fetch('https://ipwho.is/').then(r=>r.json()).then(loc=>{"
  "if(loc.success&&loc.latitude)return{lat:loc.latitude,lon:loc.longitude,city:loc.city};"
  "throw 0})"
  ".catch(()=>fetch('http://ip-api.com/json/').then(r=>r.json()).then(loc=>{"
  "if(loc.status==='success')return{lat:loc.lat,lon:loc.lon,city:loc.city};"
  "throw 0}))}"
  "function loadWeather(){fetchLocation().then(loc=>"
  "fetch('https://api.open-meteo.com/v1/forecast?latitude='+loc.lat+'&longitude='+loc.lon"
  "+'&current_weather=true').then(r=>r.json()).then(w=>{"
  "let cw=w.current_weather;let desc=WEATHER_CODES[cw.weathercode]||'';"
  "document.getElementById('weatherInfo').innerText="
  "loc.city+': '+Math.round(cw.temperature)+'°C, '+desc}))"
  ".catch(()=>{})}"
  // Раз в 30 минут — погода не меняется поминутно, незачем дёргать сторонние сервисы чаще
  "loadWeather();setInterval(loadWeather,1800000);"
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

// Прогресс трека для JS-полоски на странице (см. arylic_metadata.h, pollTrack() в PAGE_HTML) —
// pos/age в мс, JS сам считает pos+age как позицию на момент этого ответа и дальше тикает
// локально до следующего опроса
// "\"/\\" в тексте трека/URL обложки экранируются одним и тем же образом для JSON — общий
// хелпер вместо дублирования цикла на каждое поле
static String jsonEscape(const String& raw) {
  String escaped;
  escaped.reserve(raw.length());
  for (unsigned int i = 0; i < raw.length(); i++) {
    char c = raw[i];
    if (c == '"' || c == '\\') escaped += '\\';
    escaped += c;
  }
  return escaped;
}

static void handleTrack() {
  String resp = "{\"playing\":";
  resp += arylicTrackIsPlaying() ? "true" : "false";
  resp += ",\"text\":\"";
  resp += jsonEscape(arylicTrackText());
  resp += "\",\"art\":\"";
  resp += jsonEscape(arylicTrackArtUrl());
  resp += "\",\"source\":\"";
  resp += jsonEscape(arylicTrackSourceName());
  resp += "\",\"pos\":";
  resp += String(arylicTrackPosMs());
  resp += ",\"len\":";
  resp += String(arylicTrackLenMs());
  resp += ",\"age\":";
  resp += String(arylicTrackAgeMs());
  resp += ",\"vol\":";
  resp += String(arylicCurrentVolume()); // -1, если ещё неизвестна — JS это условие проверяет
  resp += "}";
  server.send(200, "application/json", resp);
}

// Разрешённые действия управления воспроизведением — белый список, чтобы в API Arylic не
// ушло что попало из query-параметра запроса (см. arylicSendPlayerCommand() —
// подтверждено live 2026-09-12, что play/pause/next/prev реально доходят и до AirPlay-сессии
// на телефоне через его обратный канал, не только до нативных интеграций вроде Spotify Connect)
static const char* const PLAYBACK_ACTIONS[] = {"onepause", "next", "prev"};
static const uint8_t PLAYBACK_ACTIONS_COUNT = sizeof(PLAYBACK_ACTIONS) / sizeof(PLAYBACK_ACTIONS[0]);

static void handlePlayback() {
  if (!server.hasArg("action")) {
    server.send(400, "text/plain", "no action");
    return;
  }
  String action = server.arg("action");
  for (uint8_t i = 0; i < PLAYBACK_ACTIONS_COUNT; i++) {
    if (action == PLAYBACK_ACTIONS[i]) {
      arylicSendPlayerCommand(PLAYBACK_ACTIONS[i]);
      server.send(204);
      return;
    }
  }
  server.send(400, "text/plain", "unknown action");
}

// Громкость самого усилителя Arylic (не громкость на телефоне) — работает независимо от
// источника, в отличие от play/pause/next/prev
static void handleVolume() {
  if (!server.hasArg("value")) {
    server.send(400, "text/plain", "no value");
    return;
  }
  arylicSetVolume(server.arg("value").toInt());
  server.send(204);
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
  server.on("/track", handleTrack);
  server.on("/playback", handlePlayback);
  server.on("/volume", handleVolume);
  server.onNotFound(handleNotFound);
  server.begin();
}

void webControlPoll() {
  server.handleClient();
}
