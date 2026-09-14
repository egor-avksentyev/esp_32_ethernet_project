#include "web_control.h"
#include "config.h"
#include "mega_link.h"
#include "wifi_setup.h"
#include "arylic_metadata.h"
#include <WebServer.h>
#include <WiFi.h>

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
  // touch-action:manipulation — убирает задержку/жест двойного тапа-зума на мобильных браузерах,
  // из-за которой быстрый второй тап по кнопке мог не долетать до click вообще (актуально для
  // playerCmd() — двойной клик на "назад" должен реально дойти как два отдельных клика)
  "button{font-size:1.3em;margin:6px;padding:14px 22px;border-radius:8px;border:none;background:#333;color:#eee;touch-action:manipulation}"
  "button:active{background:#555}"
  "button:disabled{opacity:.5}"
  // Класс, не атрибут disabled — на реально disabled-кнопке браузер вообще не диспетчеризует
  // click, а второй клик двойного клика должен ДОЙТИ до JS и встать в playerCmdQueued (см.
  // playerCmd() ниже), просто визуально "притушенным" на время запроса
  "button.pending{opacity:.5}"
  "#status{margin:12px;font-size:1.1em;color:#8cf}"
  "</style></head><body>"
  // flex+justify-content:flex-end вместо position:absolute — остаётся в потоке документа, так
  // заголовок ниже сам сдвинется, не нужно вручную резервировать место под этот блок (а на
  // мобильной ширине блок бы точно съезжал по высоте из-за переноса строк). max-width — чтобы
  // при длинном названии города блок не растягивался на всю ширину экрана, а переносился внутри
  // своих 65%, оставаясь прижатым к правому краю
  "<div style='display:flex;justify-content:flex-end;padding:6px 10px 0'>"
  "<div style='text-align:right;max-width:65%'>"
  "<span id=dtClock style='font-size:.85em;color:#aaa'>--:--:--</span>"
  "<span style='font-size:.85em;color:#aaa'> &middot; </span>"
  "<span id=dtDate style='font-size:.85em;color:#aaa'></span>"
  "<div id=weatherInfo style='margin-top:4px'>"
  "<span id=weatherIcon style='font-size:1.8em;vertical-align:middle'></span> "
  "<span id=weatherText style='font-size:1.15em;color:#8cf;vertical-align:middle'></span>"
  "</div></div></div>"
  "<h2>Preamp Remote Control</h2>"
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
  "<img id=trackArt style='display:none;max-width:240px;border-radius:6px;margin:0 auto 6px'>"
  "<div id=trackSource style='font-size:.8em;color:#8cf;display:none'></div>"
  "<div id=trackTitle style='font-size:2.1em;color:#ccc;margin-bottom:4px'></div>"
  "<div id=trackProgress>"
  // type=range вместо статичного div-бара — можно тащить пальцем/мышью, чтобы перемотать
  // (см. seekTrack() ниже). max в мс, обновляется на каждом опросе под фактическую длину трека.
  // touch-action:none — без него мобильный браузер на тонком слайдере может принять драг за
  // жест вертикального скролла страницы и не давать сдвинуть ползунок пальцем вообще (мышью
  // на десктопе этой проблемы нет, там всё, что не button/link, скроллу не мешает)
  // ontouchmove продублирован рядом с oninput не просто так: на части мобильных браузеров
  // (замечено live) сам ползунок при драге пальцем двигается нативно, а вот JS-событие
  // "input" во время движения либо не стреляет вообще, либо сильно троттлится — счётчик
  // застывал, хотя визуально палец уже сдвинул ползунок. touchmove не зависит от этого багa
  // pointerup вместо (или рядом с) "change" — на части мобильных браузеров "change" у range
  // стреляет НЕСКОЛЬКО раз за одно перетаскивание, а не один раз в конце, из-за чего
  // seekTrack() запускал несколько параллельных запросов, и самый ранний из них мог
  // завершиться (сбросить trackSeekDragging) ПОСРЕДИ ещё не законченного драга. pointerup —
  // один раз на весь жест (мышь/тач/перо — единый API), onchange оставлен только как фолбэк
  // для клавиатуры (стрелки на сфокусированном range — там pointerup не будет вообще)
  "<input id=trackSeek type=range min=0 max=1000 value=0 "
  "style='width:70%;accent-color:#8cf;touch-action:none' "
  "oninput=previewSeek(this.value) ontouchmove=previewSeek(this.value) "
  "onpointerdown='trackSeekDragging=true' onpointerup=seekTrack(this.value) "
  "onchange=seekTrack(this.value)>"
  "<div style='font-size:.8em;color:#888;margin-top:2px'>"
  "<span id=trackCur>0:00</span> / <span id=trackLen>0:00</span></div>"
  "</div></div>"
  "<div id=playbackWrap style='margin-top:10px;display:none'>"
  "<button class=playerBtn onclick=playerCmd('prev',this)>&laquo;</button>"
  "<button class=playerBtn onclick=playerCmd('onepause',this)>Play/Pause</button>"
  "<button class=playerBtn onclick=playerCmd('next',this)>&raquo;</button>"
  "<div style='margin-top:8px'>"
  "<input id=volSlider type=range min=0 max=100 value=50 style='width:18%;touch-action:none' "
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
  "setInterval(pollArylic,500);pollArylic();"
  // play/pause через "onepause" — сам переключает состояние, не полагаясь на то, что
  // getPlayerStatus считает текущим (это поле неточное для AirPlay, см. arylic_metadata.h).
  // TLS-хендшейк ESP32 -> Arylic на команду занимает ~1.5-2с (замечено live, соединение не
  // держится между кликами) — без визуального отклика кажется, что кнопка не сработала.
  // Двойной клик на "назад" — рабочий сценарий (у Arylic одно "prev" перематывает текущий
  // трек на начало, а второе подряд реально переключает на предыдущий, как на физическом
  // пульте) — поэтому второй клик, пришедшийся на занятое окно, не отбрасывается, а встаёт в
  // playerCmdQueued и уходит сразу же, как только освободится canal (см. .finally() ниже).
  // Больше одного в очереди не копим — новый клик поверх уже стоящего в очереди просто
  // заменяет его (не нужно копить длинную очередь одинаковых нажатий)
  "let playerCmdBusy=false,playerCmdQueued=null;"
  "function playerCmd(a,btn){"
  "if(playerCmdBusy){playerCmdQueued={a,btn};return}"
  "playerCmdBusy=true;"
  "let all=document.querySelectorAll('.playerBtn');"
  "all.forEach(b=>b.classList.add('pending'));"
  "let orig=btn.innerHTML;btn.innerHTML='&hellip;';"
  "fetch('/playback?action='+a).finally(()=>{"
  "playerCmdBusy=false;all.forEach(b=>b.classList.remove('pending'));btn.innerHTML=orig;"
  "if(playerCmdQueued){let q=playerCmdQueued;playerCmdQueued=null;playerCmd(q.a,q.btn)}"
  "})}"
  // volDragging блокирует перезапись ползунка живым опросом, пока палец/курсор ещё на нём —
  // без этого ползунок дёргался бы обратно к старому значению между отпусканием и applied-ответом
  "let volDragging=false;"
  "function setVolume(v){fetch('/volume?value='+v);setTimeout(()=>{volDragging=false},1000)}"
  // Прогресс трека: /track опрашивается раз в ARYLIC_POLL_INTERVAL_MS (как всё остальное), а
  // между опросами позиция досчитывается локально по реальному прошедшему времени (Date.now()),
  // чтобы полоска ехала плавно, а не прыгала — см. arylic_metadata.h за объяснением age
  "let trackPos=0,trackLen=0,trackFetchTime=0,trackPlayingNow=false,trackSeekDragging=false;"
  "function fmtTime(ms){let s=Math.max(0,Math.floor(ms/1000));let m=Math.floor(s/60);s=s%60;"
  "return m+':'+(s<10?'0':'')+s}"
  // hasTrack — трек ли играет ПРЯМО СЕЙЧАС, или он просто на паузе (j.text/j.art остаются
  // заполнены сервером и на паузе, см. arylic_metadata.cpp, ветка "не играет" — очищаются
  // только когда Arylic реально пропал из сети, не на обычной паузе кнопкой Play/Pause).
  // Поэтому обложку/заголовок/источник показываем по наличию данных, а не по j.playing —
  // иначе они бы гасли на каждую паузу, что и так видно на паузе (не нужно)
  "function pollTrack(){fetch('/track').then(r=>r.json()).then(j=>{"
  "trackPlayingNow=j.playing;trackLen=j.len;"
  // Та же защита, что у громкости чуть ниже (!volDragging) — раньше её тут не было вообще,
  // и эта строка каждые 500мс безусловно перезаписывала trackPos/trackFetchTime сырыми
  // серверными данными, включая момент сразу после перемотки, пока Arylic/бэкграунд-опрос
  // ещё не успели догнать новую позицию — отсюда и был откат назад независимо от устройства
  // (десктоп/мобильный тут ни при чём, дело было именно в этом)
  "if(!trackSeekDragging){trackPos=j.pos+j.age;trackFetchTime=Date.now()}"
  "let hasTrack=j.playing||j.text||j.art;"
  "document.getElementById('trackWrap').style.display=hasTrack?'block':'none';"
  // AirPlay не отдаёт Title/Artist вообще (см. arylic_metadata.h) — j.text тогда всегда "".
  // Название трека тут не показываем совсем (источник и так виден отдельной строкой,
  // #trackSource, ниже) — просто прячем заголовок, а не подставляем туда что-то ещё
  "let t=document.getElementById('trackTitle');"
  "if(j.text){t.innerText=j.text;t.style.display='block'}else{t.style.display='none'}"
  // Источник (Spotify/AirPlay/...) — отдельная строка, показывается независимо от того,
  // распарсились ли title/artist (AirPlay их не отдаёт вообще, но источник знать можно)
  // Крупно — когда title/artist нет вообще (AirPlay, trackTitle тогда скрыт выше): это
  // единственный видимый текст "что сейчас играет". Размер задаём JS-ом напрямую в
  // src.style, а не CSS-классом — у элемента уже есть встроенный style='font-size:...'
  // (задаёт БАЗОВЫЙ маленький размер), а инлайновый style всегда перебивает правило класса
  // из <style>, так что class.toggle тут ни на что не влиял
  "let src=document.getElementById('trackSource');"
  "if(j.source){src.innerText=j.source;src.style.display='block';"
  "src.style.fontSize=j.text?'.8em':'2.1em';"
  "src.style.fontWeight=j.text?'normal':'bold'}"
  "else{src.style.display='none'}"
  // Позиция трека для AirPlay не двигается вообще (устройство её не отдаёт ни в одном
  // известном API, проверено live — см. arylic_metadata.h) — полоска бы просто застыла на
  // месте и вводила в заблуждение, поэтому для этого источника прячем её целиком
  "document.getElementById('trackProgress').style.display=(j.source==='AirPlay')?'none':'block';"
  // Обложка отдаётся ссылкой на CDN сервиса-источника, не байтами — сам img её и грузит.
  // Пусто, если сервис её не отдаёт (например AirPlay/Apple Music, см. arylic_metadata.h)
  "let art=document.getElementById('trackArt');"
  "if(j.art){if(art.src!==j.art)art.src=j.art;art.style.display='block'}"
  "else{art.style.display='none'}"
  "if(!volDragging&&j.vol>=0)document.getElementById('volSlider').value=j.vol})}"
  "function tickTrack(){"
  "document.getElementById('trackSeek').max=trackLen;"
  "if(!trackPlayingNow||trackSeekDragging)return;"
  "let pos=trackPos+(Date.now()-trackFetchTime);if(trackLen>0&&pos>trackLen)pos=trackLen;"
  "document.getElementById('trackCur').innerText=fmtTime(pos);"
  "document.getElementById('trackLen').innerText=fmtTime(trackLen);"
  "document.getElementById('trackSeek').value=pos}"
  // Пока тащишь ползунок — trackCur показывает время ПОД ползунком (куда попадёшь), не
  // застывшее время последнего опроса. tickTrack() выше не трогает trackCur, пока
  // trackSeekDragging — иначе эти два обновления дрались бы друг с другом
  "function previewSeek(v){trackSeekDragging=true;document.getElementById('trackCur').innerText=fmtTime(v)}"
  // Отпустили ползунок — шлём перемотку и держим trackSeekDragging=true, пока запрос реально
  // не завершится (.finally, не setTimeout с угаданной задержкой — раньше от него была
  // отдельная гонка, если "change" стрелял больше одного раза за перетаскивание).
  //
  // trackPos/trackFetchTime обновляем ОПТИМИСТИЧНО, сразу же — до сих пор они хранили позицию
  // из ПОСЛЕДНЕГО /track-опроса (ещё до перемотки), и как только trackSeekDragging становится
  // false, tickTrack() тут же начинал интерполировать именно от неё — ползунок откатывался
  // назад, пока следующий опрос (до 500мс) не подтягивал актуальную позицию с Arylic. Отсюда и
  // были скачки "новое место -> старое -> новое": откат был реальный, просто короткий
  //
  // seekInFlight — у элемента теперь два обработчика на отпускание (onpointerdown/up и
  // onchange, см. PAGE_HTML — второй как фолбэк для клавиатуры), на некоторых браузерах могут
  // сработать оба почти одновременно. Без этой защиты каждый запускал бы свой fetch, и более
  // ранний мог бы завершиться (сбросить trackSeekDragging) раньше более позднего — тот же
  // класс гонки, что был с несколькими "change" за одно перетаскивание. Повторный вызов, пока
  // запрос уже летит, просто обновляет оптимистичную позицию и не трогает fetch/dragging
  "let seekInFlight=false;"
  "function seekTrack(v){trackSeekDragging=true;trackPos=Number(v);trackFetchTime=Date.now();"
  "if(seekInFlight)return;seekInFlight=true;"
  "fetch('/seek?pos='+v).finally(()=>{seekInFlight=false;"
  // Проверено live через curl: /seek подтверждается ЗА СЕБЯ (~1.7-2с TLS), но фоновый опрос
  // Arylic на самом ESP32 (arylicPollTask, отдельный цикл раз в ARYLIC_POLL_INTERVAL_MS) в
  // этот момент ещё не в курсе — реально нужно ЕЩЁ 1-2 таких цикла после ответа /seek, чтобы
  // /track начал отдавать новую позицию. Если снять trackSeekDragging сразу по .finally(),
  // ближайший /track ответ ещё вернёт СТАРУЮ позицию, и pollTrack() (защищённый выше) снова
  // её применит, как только флаг снимется — ползунок откатится назад, а через ещё пару
  // опросов снова прыгнет вперёд. Задержка ниже — запас на то, чтобы бэкенд сам догнал"
  "setTimeout(()=>{trackSeekDragging=false},1500)})}"
  "setInterval(pollTrack,500);pollTrack();setInterval(tickTrack,500);"
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
  // Иконка по тому же коду WMO — отдельная таблица, не завязана на текст описания
  "const WEATHER_ICONS={0:'☀️',1:'☀️',2:'⛅',3:'☁️',"
  "45:'🌫️',48:'🌫️',51:'🌦️',53:'🌦️',"
  "55:'🌦️',56:'🌨️',57:'🌨️',61:'🌧️',"
  "63:'🌧️',65:'🌧️',66:'🌨️',67:'🌨️',"
  "71:'🌨️',73:'🌨️',75:'🌨️',77:'🌨️',"
  "80:'🌦️',81:'🌧️',82:'🌧️',85:'🌨️',"
  "86:'🌨️',95:'⛈️',96:'⛈️',99:'⛈️'};"
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
  "document.getElementById('weatherIcon').innerText=WEATHER_ICONS[cw.weathercode]||'';"
  "document.getElementById('weatherText').innerText="
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
      // Пробрасываем реальный результат (arylicSendPlayerCommand() реально ходит на Arylic
      // по HTTPS) — раньше ESP32 отвечал 204 независимо от исхода, и если Arylic был
      // временно недоступен (mDNS/TLS сбой, см. arylic_metadata.cpp), команда молча
      // терялась, а кнопка на веб-странице выглядела нажатой
      if (!arylicSendPlayerCommand(PLAYBACK_ACTIONS[i])) {
        server.send(502, "text/plain", "arylic unreachable");
        return;
      }
      if (action == "onepause") {
        // Оптимистичный оверрайд PLAY:-состояния — см. arylicNotifyOnepausePressed() за тем,
        // почему опрос сам по себе не может это заметить для AirPlay ("status" не меняется
        // на паузе, проверено live)
        arylicNotifyOnepausePressed();
      }
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
  if (!arylicSetVolume(server.arg("value").toInt())) {
    server.send(502, "text/plain", "arylic unreachable");
    return;
  }
  server.send(204);
}

// Перемотка (drag полосы прогресса на веб-странице, см. PAGE_HTML — trackSeek). pos — мс,
// arylicSeek() сама переводит в секунды для команды LinkPlay. НЕ проверено живьём на этом
// устройстве (см. комментарий у arylicSeek() в arylic_metadata.h) — если Arylic эту команду
// не поддерживает, сюда просто придёт не-OK ответ и вернётся 502, как при любой другой
// неудачной команде
static void handleSeek() {
  if (!server.hasArg("pos")) {
    server.send(400, "text/plain", "no pos");
    return;
  }
  if (!arylicSeek(server.arg("pos").toInt())) {
    server.send(502, "text/plain", "arylic unreachable или не поддерживает seek");
    return;
  }
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
  server.on("/seek", handleSeek);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.print("[web] веб-морда поднята, зайди на http://");
  Serial.print(WiFi.localIP());
  Serial.print(":");
  Serial.println(WEB_SERVER_PORT);
}

void webControlPoll() {
  server.handleClient();
}
