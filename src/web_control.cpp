#include "web_control.h"
#include "config.h"
#include "mega_link.h"
#include "wifi_setup.h"
#include "arylic_metadata.h"
#include <WebServer.h>
#include <WiFi.h>

static WebServer server(WEB_SERVER_PORT);

// Последняя команда, отправленная на Mega. До 2026-09-21 было единственное, что ESP32 вообще
// "знал" о состоянии системы (Mega ничего не отправляла назад) — теперь webPoweredOff ниже
// отражает настоящее состояние питания Mega (см. POWER: в mega_link.h), но меню/громкость/
// mute по-прежнему неизвестны ESP32 — см. README.md, "Известное ограничение: статус на веб-странице"
static char lastActionSent = '\0';

// Что показывает ЭТА страница ("основной интерфейс" или большая кнопка "Power On"). С
// 2026-09-21 отражает настоящее состояние питания Mega (см. applyWebPowerState() ниже,
// вызывается и из handleCmd() при нажатии кнопки на самой странице, и из mega_link.cpp при
// получении POWER: по UART) — раньше было чисто оптимистичным отражением последнего клика на
// этой же странице, без понятия о реальном пульте. Флаг здесь, на ESP32, а не только в JS
// вкладки — чтобы пережить перезагрузку страницы (см. handleStatus()/applyPowerState() в
// PAGE_HTML): свежая вкладка сразу увидит то же состояние, не сбрасываясь в "включено" по умолчанию
static bool webPoweredOff = false;

// true, только если паузу при выключении поставили именно мы (см. applyWebPowerState() ниже)
// — а не если трек и так уже стоял на паузе сам по себе. Нужно, чтобы при обратном включении
// возобновлять воспроизведение ТОЛЬКО в этом случае, а не запускать музыку, которую
// пользователь сам поставил на паузу заранее
static bool pausedByPowerOff = false;

// Общая точка для обоих источников информации о питании — кнопка "Power" на самой веб-
// странице (handleCmd(), не знает о реальном состоянии Mega, шлёт CMD:P и сразу переключает
// оптимистично) и настоящий сигнал POWER: от Mega по UART (megaLinkOnPowerChanged() ниже,
// вызывается из mega_link.cpp) — оба должны одинаково гасить/зажигать страницу и
// ставить/снимать паузу Spotify через локальный Arylic API (не через Spotify Web API — тот
// требует авторизации и не знает про AirPlay/другие источники, локальная пауза Arylic работает
// независимо от того, что именно сейчас играет). Не делает ничего, если состояние не изменилось
// — иначе периодический повтор POWER: от Mega (см. esp32_link.h) заново дёргал бы паузу/резюм
// каждые несколько секунд, пока питание не менялось
void applyWebPowerState(bool off) {
  if (off == webPoweredOff) {
    return;
  }
  webPoweredOff = off;
  if (webPoweredOff) {
    // На паузу — только уходя в выключенное состояние, и только если реально играет:
    // onepause сам ПЕРЕКЛЮЧАЕТ play/pause (не имеет отдельной команды "только пауза"),
    // так что при уже стоящей паузе эта же команда включила бы воспроизведение обратно
    pausedByPowerOff = false;
    if (arylicTrackIsPlaying()) {
      if (arylicSendPlayerCommand("onepause")) {
        arylicNotifyOnepausePressed();
        pausedByPowerOff = true;
      }
    }
  } else if (pausedByPowerOff) {
    // Возобновляем, только если паузу поставили именно мы при выключении (см.
    // pausedByPowerOff выше) — иначе рискуем запустить музыку, которую пользователь
    // сам поставил на паузу заранее, ещё до выключения
    if (arylicSendPlayerCommand("onepause")) {
      arylicNotifyOnepausePressed();
    }
    pausedByPowerOff = false;
  }
}

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
  // Белые полосы сверху/снизу на iPhone (подтверждено на iPhone 13 Pro Max) — это не переливы
  // фона страницы, а собственный хром Safari (статус-бар сверху, панель адреса снизу): с iOS 15
  // Safari умеет красить их под цвет страницы через theme-color, без него — свой светлый цвет
  // по умолчанию, независимо от фона body/html
  "<meta name=theme-color content='#000000'>"
  "<title>Control</title><style>"
  // html — отдельно от body: в Safari на iOS при "резиновом" оттягивании страницы вверх/вниз
  // (rubber-band overscroll) видно фон именно html, а не body — без этого правила там был белый
  // (фон html по умолчанию), пока фон body туда не дотягивался.
  // background-attachment:fixed сюда специально НЕ добавляем (было и убрано) — ломало iOS Safari
  // само по себе (резиновая прокрутка), см. историю ниже.
  //
  // background-color:#000 ОТДЕЛЬНО от background-image(градиент) — не просто на всякий случай:
  // в iOS 26 Safari перестал читать theme-color вообще и вместо него красит статус-бар/панель
  // адреса по СВОЙСТВУ background-color (не background-image!) у body/ближайших fixed-элементов.
  // "background:linear-gradient(...)" — это чистый background-image, background-color при этом
  // остаётся не задан (прозрачный) — Safari брал свой белый по умолчанию, отсюда и белые полосы
  // сверху/снизу, не увиденные раньше на плоской заливке "background:#111" (та же short-hand
  // форма ЗАДАЁТ background-color, градиент — нет). Задаём оба свойства явно
  "html{background-color:#000;background-image:linear-gradient(135deg,#000,#3a3a3a)}"
  "body{font-family:sans-serif;text-align:center;color:#eee;min-height:100vh;"
  "background-color:#000;background-image:linear-gradient(135deg,#000,#3a3a3a)}"
  // touch-action:manipulation — убирает задержку/жест двойного тапа-зума на мобильных браузерах,
  // из-за которой быстрый второй тап по кнопке мог не долетать до click вообще (актуально для
  // playerCmd() — двойной клик на "назад" должен реально дойти как два отдельных клика).
  // -webkit-touch-callout/user-select:none — на iOS долгое удержание кнопки (см. startHold() у
  // Up/Down — именно она держится дольше обычного клика) без этого триггерит системное меню
  // выделения/копирования текста поверх кнопки, перехватывая жест вместо повторного вызова cmd()
  // background-color отдельно от background-image(градиент) — та же причина, что у html/body
  // ниже (iOS 26 Safari красит хром по background-color, не background-image, см. комментарий
  // там же за подробностями) — здесь скорее для единообразия, чем по необходимости (кнопки не
  // у самого края экрана), но безопаснее задавать оба свойства везде, где раньше был просто цвет
  "button{font-size:1.3em;margin:6px;padding:14px 22px;border-radius:8px;border:none;color:#eee;"
  "background-color:#000;background-image:linear-gradient(135deg,#000,#3a3a3a);"
  "touch-action:manipulation;-webkit-touch-callout:none;-webkit-user-select:none;user-select:none}"
  "button:active{background-image:linear-gradient(135deg,#222,#555)}"
  "button:disabled{opacity:.5}"
  // Градиентный текст (вместо сплошного синего #8cf у погоды) — светлый серебристый градиент,
  // не тон в тон с фоном (тот же чёрный-в-тёмно-серый на светлом тексте был бы попросту не
  // виден), но в той же монохромной палитре, что и весь остальной градиентный фон/кнопки/грани.
  // -webkit-background-clip нужен явно — background-clip:text до сих пор частично требует
  // префикс в Safari
  "#weatherText{background-image:linear-gradient(135deg,#fff,#999);"
  "-webkit-background-clip:text;background-clip:text;color:transparent}"
  // Класс, не атрибут disabled — на реально disabled-кнопке браузер вообще не диспетчеризует
  // click, а второй клик двойного клика должен ДОЙТИ до JS и встать в playerCmdQueued (см.
  // playerCmd() ниже), просто визуально "притушенным" на время запроса
  "button.pending{opacity:.5}"
  "select{font-size:.85em;background:#222;color:#eee;border:1px solid #444;border-radius:6px;padding:4px 6px}"
  "#status{margin:12px;font-size:1.1em;color:#8cf}"
  // Тот же приём выезжания — теперь у двух независимых раскрывашек (Remote Control и
  // Settings, см. #settingsToggle/#settingsCollapse ниже) — общие правила через запятую,
  // а не два одинаковых набора под каждый ID
  "#remoteToggle,#settingsToggle{font-size:1em;padding:10px 18px}"
  // Плавное сворачивание/разворачивание без JS-измерения высоты — CSS Grid с
  // grid-template-rows: 0fr -> 1fr, стандартный приём для анимации "auto height", которую
  // обычный max-height/transition сделать плавной не может без знания реальной высоты контента
  "#remoteCollapse,#settingsCollapse{display:grid;grid-template-rows:0fr;transition:grid-template-rows .3s ease}"
  "#remoteCollapse.open,#settingsCollapse.open{grid-template-rows:1fr}"
  "#remoteCollapse>div,#settingsCollapse>div{overflow:hidden}"
  // Обложка — три темы на выбор (см. #artThemeSelect/applyArtTheme() ниже), выбор живёт в
  // localStorage (per-viewer, как язык). .themeCircle — модификатор "как пластинка": круглая,
  // крутится, пока играет (см. pollTrack()/updatePlayVisuals()). Без этого класса (тема
  // "квадрат") — обычный статичный квадрат, animation тут вообще не задан, так что
  // animationPlayState на этот элемент в JS просто ни на что не влияет. object-fit:cover
  // держит квадратный кроп даже если реальное изображение с CDN окажется не идеально квадратным
  "#trackArt{width:200px;height:200px;margin:0 auto 6px;object-fit:cover;display:none}"
  "#trackArt.themeCircle{border-radius:50%;animation:spin 20s linear infinite;animation-play-state:paused}"
  "@keyframes spin{from{transform:rotate(0)}to{transform:rotate(360deg)}}"
  // Заглушка обложки — источники без своей картинки (AirPlay/Apple Music, см.
  // arylic_metadata.h) её никогда не отдают. Кубическую тему для заглушки не делаем (текст на
  // гранях без картинки смысла не имеет) — вместо неё всегда эта же круглая/квадратная заглушка,
  // см. renderArt(): для темы "куб" используется её же круглый вариант
  "#trackArtPlaceholder{width:200px;height:200px;margin:0 auto 6px;"
  "background:linear-gradient(135deg,#000,#3a3a3a);"
  "border:3px solid rgba(160,160,160,.4);display:none;align-items:center;justify-content:center}"
  "#trackArtPlaceholder.themeCircle{border-radius:50%;animation:spin 20s linear infinite;animation-play-state:paused}"
  "#trackArtPlaceholder span{color:rgba(160,160,160,.6);font-size:1.1em;text-align:center;padding:0 12px}"
  // 3D-куб — крутится "на месте" (сам куб не смещается по экрану — ось вращения проходит
  // через его геометрический ЦЕНТР, как обычно у 3D-трансформаций), но эта ось — не вертикаль
  // и не горизонталь, а ПРОСТРАНСТВЕННАЯ ДИАГОНАЛЬ куба (через два противоположных угла из
  // восьми — то самое "вращение на углу", которое просили, только без побочного качания влево-
  // вправо, которое давал предыдущий вариант с осью по грани). Грани строятся стандартным
  // приёмом (rotateY(угол)+translateZ(половина стороны), см. cubeFace.f0..f3) вокруг обычного
  // центра #trackArtCubeInner (transform-origin по умолчанию, не переопределяем) — только 4
  // боковые грани, верх/низ всё равно никогда не видны под этим углом обзора.
  //
  // Как это устроено (см. @keyframes cubeSpin): CSS не умеет "вращать вокруг произвольной оси"
  // одной функцией, поэтому раскладываем на пять — сначала двумя поворотами (rotateZ+rotateY)
  // разворачиваем систему координат так, что телесная диагональ куба совпадает с осью Y, потом
  // крутим по НЕЙ обычным rotateY (это и есть анимируемый угол), и в конце теми же двумя
  // поворотами в обратную сторону возвращаем куб в исходную ориентацию на экране (чтобы при
  // angle=0 он выглядел обычным ровным кубом, а не перекошенным). Угол 54.7356deg — это
  // arccos(1/√3), "магический угол" между диагональю куба и его гранью, известная константа для
  // этого приёма, не подбор на глаз
  // Контейнер куба (#trackArtCube) — 264px, крупнее, чем у круга/квадрата (200px) специально:
  // сама грань куба (F=144px) заметно меньше своего контейнера с запасом, чтобы даже самая
  // дальняя точка куба (угол, на расстоянии половины пространственной диагонали от центра,
  // F*√3/2 ≈ 125px при F=144) не доставала до края контейнера (радиус 132px) — overflow:hidden
  // ниже всё равно подстраховывает на случай перспективных искажений по краям
  "#trackArtCube{width:264px;height:264px;margin:0 auto 6px;perspective:700px;"
  "display:none;position:relative;overflow:hidden}"
  "#trackArtCubeInner{width:144px;height:144px;position:absolute;left:50%;top:50%;"
  "margin-left:-72px;margin-top:-72px;transform-style:preserve-3d;"
  "animation:cubeSpin 12s linear infinite;animation-play-state:paused}"
  // background-image задаётся из JS (renderArt()) — либо ссылка на обложку, либо градиент
  // чёрный->тёмно-серый, когда обложки нет (см. там же); своего значения в CSS нет специально,
  // инлайновый style всё равно бы его перебил
  "#trackArtCubeInner .cubeFace{position:absolute;top:0;left:0;width:144px;height:144px;"
  "background-size:cover;background-position:center;backface-visibility:hidden;"
  "display:flex;align-items:center;justify-content:center}"
  "#trackArtCubeInner .cubeFace span,#trackArtPyramidInner .pyramidFace span{"
  "color:rgba(210,210,210,.85);font-size:1em;text-align:center;padding:0 10px;pointer-events:none}"
  // f0-f3 — 4 боковые грани (перед/право/зад/лево), f4/f5 — верх/низ. Раньше вращение шло
  // только вокруг вертикали (rotateY), и верх/низ никогда не были видны, поэтому их не строили
  // вовсе — с вращением вокруг диагонали куба (см. @keyframes cubeSpin ниже) куб временами
  // наклоняется и по X/Z тоже, и без этих двух граней в эти моменты сквозь куб было видно
  // пустоту/фон контейнера (чёрные "дыры" на месте недостающих граней)
  "#trackArtCubeInner .cubeFace.f0{transform:translateZ(72px)}"
  "#trackArtCubeInner .cubeFace.f1{transform:rotateY(90deg) translateZ(72px)}"
  "#trackArtCubeInner .cubeFace.f2{transform:rotateY(180deg) translateZ(72px)}"
  "#trackArtCubeInner .cubeFace.f3{transform:rotateY(-90deg) translateZ(72px)}"
  "#trackArtCubeInner .cubeFace.f4{transform:rotateX(90deg) translateZ(72px)}"
  "#trackArtCubeInner .cubeFace.f5{transform:rotateX(-90deg) translateZ(72px)}"
  "@keyframes cubeSpin{"
  "from{transform:rotateY(-45deg) rotateZ(-54.7356deg) rotateY(0deg) rotateZ(54.7356deg) rotateY(45deg)}"
  "to{transform:rotateY(-45deg) rotateZ(-54.7356deg) rotateY(360deg) rotateZ(54.7356deg) rotateY(45deg)}"
  "}"
  // 4-гранная пирамида — ось вращения проходит через вершину (rotateY на #trackArtPyramidInner,
  // которая сама — точка нулевого размера в месте вершины, см. ниже), а раз вершина правильной
  // пирамиды и так лежит на одной вертикали с центром основания, это и есть её обычная
  // вертикальная ось симметрии — в отличие от куба, тут не нужен трюк с наклонной осью, обычный
  // rotateY уже "крутится на месте".
  //
  // База (B=140px) и высота (H=150px) заданы так, чтобы боковая грань (треугольник, вырезанный
  // clip-path из прямоугольника BxL) при развороте попадала под нужным углом: L — высота этого
  // треугольника (расстояние от вершины до середины стороны основания, L=√(H²+(B/2)²)≈166px),
  // угол наклона грани от вертикали — arctan((B/2)/H)≈25°. #trackArtPyramidInner — сама точка
  // нулевого размера в вершине; каждая грань висит от неё вниз (transform-origin:50% 0% — верх
  // грани = вершина), сперва наклоняется наружу на 25° (rotateX), потом разворачивается в одну
  // из 4 сторон света (rotateY) — тот же порядок применения (снаружи внутрь: сначала то, что
  // правее в списке), что и у translateZ+rotateY на гранях куба выше
  "#trackArtPyramid{width:220px;height:220px;margin:0 auto 6px;perspective:700px;"
  "display:none;position:relative;overflow:hidden}"
  "#trackArtPyramidInner{position:absolute;left:50%;top:35px;width:0;height:0;"
  "transform-style:preserve-3d;animation:pyramidSpin 12s linear infinite;animation-play-state:paused}"
  "#trackArtPyramidInner .pyramidFace{position:absolute;left:-70px;top:0;width:140px;height:166px;"
  "background-size:cover;background-position:center;backface-visibility:hidden;"
  "transform-origin:50% 0%;clip-path:polygon(50% 0%,0% 100%,100% 100%);"
  "display:flex;align-items:flex-end;justify-content:center;padding-bottom:20%;box-sizing:border-box}"
  "#trackArtPyramidInner .pyramidFace.p0{transform:rotateY(0) rotateX(25deg)}"
  "#trackArtPyramidInner .pyramidFace.p1{transform:rotateY(90deg) rotateX(25deg)}"
  "#trackArtPyramidInner .pyramidFace.p2{transform:rotateY(180deg) rotateX(25deg)}"
  "#trackArtPyramidInner .pyramidFace.p3{transform:rotateY(-90deg) rotateX(25deg)}"
  "@keyframes pyramidSpin{from{transform:rotateY(0)}to{transform:rotateY(360deg)}}"
  // Прыгающий эквалайзер слева экрана — position:fixed на всю высоту вьюпорта (виден всегда,
  // не часть потока страницы), pointer-events:none — чтобы не перехватывал тапы/клики по
  // реальным элементам управления под ним. Горизонтальные (растут в ширину от левого края) —
  // вертикальные тонкие столбики было плохо видно; серый и полупрозрачный (rgba, не #8cf) —
  // чтобы сочетался с чёрным фоном, а не бросался в глаза ярким цветом. 1см — реальная
  // физическая единица CSS (не px) — сама переводится браузером в пиксели под фактическое
  // разрешение экрана, ровно то ограничение, что попросили. Сами столбики (много, на всю
  // высоту) генерируются в JS (см. buildEqualizer() ниже) — вручную перечислять десятки
  // nth-child правил тут не стоило бы своей сложности
  "#equalizer{position:fixed;left:0;top:0;height:100vh;width:1cm;pointer-events:none}"
  "#equalizer .eqBar{position:absolute;left:0;height:4px;background:rgba(160,160,160,.4);"
  "border-radius:2px;width:3px;animation:eqBounce 1s ease-in-out infinite;animation-play-state:paused}"
  // Как и у трека выше — крутится/скачет, только пока реально играет
  "#equalizer.playing .eqBar{animation-play-state:running}"
  "@keyframes eqBounce{0%,100%{width:3px}50%{width:1cm}}"
  // Экран Spotify (#spotifyScreen) — полноэкранный, подменяет собой #controlWrap целиком (см.
  // showSpotifyScreen()/showControlScreen() в <script>), переключение мгновенное, без перехода
  // на другой сайт: логин — единственный момент, когда браузер ненадолго улетает на
  // login.html/callback.html (GitHub Pages, нужен только из-за https-требования Spotify к
  // redirect_uri, см. CLAUDE.md/README) и тут же возвращается сюда же с токеном во фрагменте
  // адреса (см. spCaptureCallback() в <script> ниже) — сама страница с этого момента работает
  // целиком тут же, без повторных переходов
  // flex-wrap — вкладок набралось много (поиск/треки/альбомы/плейлисты/недавнее/топ+Выйти),
  // без переноса они бы вылезали за край экрана на телефоне вместо аккуратного переноса строки
  // Общий эффект нажатия для ВСЕХ кнопок на экране Spotify. :active сам по себе оказался
  // недостаточен — на телефонах (особенно iOS Safari) браузер по умолчанию вообще не
  // применяет :active к элементам без своего touch-обработчика (известная особенность
  // WebKit, экономия ресурсов) — эффект появлялся на десктопе с мышью, но не на реальном
  // устройстве. .pressed — тот же эффект, но выставляется/снимается вручную через JS
  // (setupSpTapFeedback() ниже, pointerdown/pointerup/pointercancel) — работает везде
  // одинаково, не полагаясь на то, подхватит ли браузер :active сам
  "#spotifyScreen button{transition:transform .08s}"
  "#spotifyScreen button:active,#spotifyScreen button.pressed{transform:scale(.9)}"
  "#spotifyScreen .spTabs{display:flex;flex-wrap:wrap;gap:6px;align-items:center;margin-bottom:8px}"
  "#spotifyScreen .spTabs button{font-size:.9em;padding:6px 12px;border-radius:16px}"
  "#spotifyScreen .spTabs button.active{border-color:#8cf;color:#8cf}"
  // Мини-переключатели внутри вкладки "Топ" (Треки/Исполнители/Подписки) — тот же принцип
  // активной подсветки, что и у больших вкладок выше, но компактнее
  ".spSubTabs{display:flex;gap:6px;align-items:center;margin-bottom:8px}"
  ".spSubTabs button{font-size:.8em;padding:4px 10px;border-radius:14px}"
  ".spSubTabs button.active{border-color:#8cf;color:#8cf}"
  ".spPrimaryBtn{border-color:#8cf;color:#8cf}"
  ".spSearchBox{display:flex;gap:6px;margin-bottom:8px}"
  ".spSearchBox input{flex:1;font-size:1em;padding:10px;border-radius:6px;border:none;background:#222;color:#eee}"
  ".spRowList{display:flex;flex-direction:column;gap:2px;max-height:340px;overflow-y:auto}"
  // :active — сильнее прежнего (.08 -> .18 альфа) + лёгкое сжатие transform'ом, раньше
  // нажатие на строку было почти незаметно, особенно на телефоне
  ".spRow{display:flex;align-items:center;gap:10px;padding:6px 2px;border-radius:6px;"
  "text-align:left;transition:transform .08s}"
  ".spRow:active,.spRow.pressed{background:rgba(255,255,255,.18);transform:scale(.98)}"
  ".spThumb{width:46px;height:46px;border-radius:4px;object-fit:cover;background:#222;flex-shrink:0}"
  ".spMeta{flex:1;min-width:0}"
  ".spName{font-size:1.01em;color:#eee;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
  ".spSub{font-size:.78em;color:#999;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
  // Сердечко — button.iconBtn общего вида нет в этом файле (кнопки тут все прямоугольные),
  // делаем компактную круглую без рамки прямо тут же, по образцу остальных инлайновых стилей
  ".spHeart{border:none;background:none;padding:4px;border-radius:50%;flex-shrink:0}"
  ".spHeart svg{fill:none;stroke:#999;stroke-width:2}"
  ".spHeart.saved svg{fill:#8cf;stroke:#8cf}"
  // "+В очередь"/"+В плейлист" — короткие текстовые кнопки-таблетки вместо ещё пары
  // самодельных SVG-иконок рядом с сердечком: в ряду итак тесно (обложка+текст+3 кнопки),
  // текст компактнее и не требует рисовать новую иконку, которую негде превью проверить
  ".spPillBtn{border:none;background:none;padding:3px 7px;border-radius:10px;"
  "font-size:.68em;color:#999;flex-shrink:0;white-space:nowrap}"
  ".spPillBtn:active{background:rgba(255,255,255,.08)}"
  ".spEmpty{color:#888;text-align:center;padding:14px 0;font-size:.85em}"
  ".spSectionTitle{font-size:1em;color:#ccc;margin:14px 0 6px}"
  ".spThumb.spThumbRound{border-radius:50%}"
  ".spDeviceTag{margin-top:8px;font-size:.75em;color:#8cf;text-align:right}"
  // Мини-плеер — вне #mainContent (см. разметку ниже), поэтому виден независимо от состояния
  // питания Mega и от того, открыта ли вкладка Spotify — воспроизведение через Arylic как
  // Spotify Connect-цель не зависит ни от того, ни от другого
  "#spPlayerBar{position:fixed;left:0;right:0;bottom:0;padding:8px 12px;"
  "background:rgba(10,10,10,.92);border-top:1px solid rgba(160,160,160,.25);z-index:6}"
  "#spPlayerBar .spNpRow{display:flex;align-items:center;gap:10px}"
  "#spPlayerBar .spNpMeta{flex:1;min-width:0}"
  "#spPlayerBar .spNpMeta .spName,#spPlayerBar .spNpMeta .spSub{display:block}"
  "#spPlayerBar .spTransport{display:flex;align-items:center;gap:2px}"
  "#spPlayerBar .spTransport button{border:none;background:none;padding:6px;border-radius:50%}"
  "#spPlayerBar .spTransport button svg{fill:#eee}"
  // Shuffle/repeat — те же круглые кнопки, что play/pause/prev/next, но текстовые эмодзи вместо
  // SVG (проще и безопаснее, чем рисовать ещё два path вручную в escaped-строке) — .active даёт
  // подсветку фоном (не текстом: цветные эмодзи-глифы игнорируют CSS color на большинстве систем)
  "#spPlayerBar .spTransport button.spToggleBtn{font-size:.85em}"
  "#spPlayerBar .spTransport button.active{background:rgba(140,204,255,.25)}"
  "#spPlayerBar .spSeekRow{display:flex;align-items:center;gap:6px;margin-top:4px}"
  "#spPlayerBar .spSeekRow span{font-size:.7em;color:#888;width:30px;text-align:center;flex-shrink:0}"
  "#spPlayerBar input[type=range]{width:100%;accent-color:#8cf;touch-action:none}"
  "#spPlayerBar .spVolRow{display:flex;align-items:center;gap:8px;margin-top:2px}"
  "#spPlayerBar .spVolRow input[type=range]{width:90px}"
  "</style></head><body>"
  // #controlWrap — все существующие экраны (эквалайзер, шапка, меню, экран Power Off) одним
  // блоком, чтобы вкладка Spotify (см. #spotifyScreen ниже) могла спрятать их разом одним
  // toggle, вместо того чтобы помнить, что именно из них сейчас показано (mainContent или
  // powerOffScreen решает applyPowerState() — showControlScreen() просто зовёт её заново)
  "<div id=controlWrap>"
  "<div id=equalizer></div>"
  // justify-content:space-between — слева выбор языка, справа дата/погода (было flex-end,
  // держало только правый блок). max-width на правом блоке — чтобы длинное название города
  // не растягивало его на всю ширину экрана, а переносилось внутри своих 55%
  "<div style='display:flex;justify-content:space-between;align-items:flex-start;padding:6px 10px 0'>"
  // Флаг — просто эмодзи-текст внутри <option>, работает без картинок/доп. разметки.
  // Порядок — как попросили: украинский первым, русский последним (английский/румынский
  // между ними — порядок для них отдельно не оговаривался)
  "<select id=langSelect onchange=applyLanguage(this.value)>"
  "<option value=uk>&#127482;&#127462; Українська</option>"
  "<option value=en>&#127468;&#127463; English</option>"
  "<option value=ro>&#127479;&#127476; Română</option>"
  "<option value=ru>&#127479;&#127482; Русский</option>"
  "</select>"
  "<div style='text-align:right;max-width:55%'>"
  "<span id=dtClock style='font-size:.85em;color:#aaa'>--:--:--</span>"
  "<span style='font-size:.85em;color:#aaa'> &middot; </span>"
  "<span id=dtDate style='font-size:.85em;color:#aaa'></span>"
  "<div id=weatherInfo style='margin-top:4px'>"
  "<span id=weatherIcon style='font-size:1.8em;vertical-align:middle'></span> "
  "<span id=weatherText style='font-size:1.15em;vertical-align:middle'></span>"
  "</div></div></div>"
  // Кнопка входа в Spotify — всегда на виду (не внутри #mainContent), сразу под шапкой, а не
  // спрятана среди Remote Control/Settings: по вашей задумке это отдельный полноэкранный режим
  // (см. #spotifyScreen/showSpotifyScreen() ниже), а не ещё одна раскрывашка внутри обычного
  // экрана — переключение полностью на этой же странице, без перехода на другой сайт
  "<div style='text-align:center;padding:6px 0 0'>"
  "<button onclick=showSpotifyScreen() style='font-size:1em;padding:8px 20px'>"
  "&#127925; Spotify</button></div>"
  // Всё "живое" содержимое страницы (тембр-блок, трек, плеер, ручной IP, смена Wi-Fi) — внутри
  // одного контейнера, чтобы одним переключением видимости (см. applyPowerState() ниже) убрать
  // его целиком при выключении питания, оставив только то, что явно должно остаться (язык/
  // дата/погода в шапке — вне этого div, см. выше) и большую кнопку "Power On" (#powerOffScreen,
  // тоже вне этого div, см. ниже)
  "<div id=mainContent>"
  // Тембр-блок (левая/правая/энтер, вверх/вниз, mute/source/power) теперь скрыт за этой
  // кнопкой — раньше был всегда виден под заголовком "Preamp Remote Control", теперь сам
  // заголовок стал кнопкой-раскрывашкой (см. toggleCollapse()/#remoteCollapse в <style>)
  "<button id=remoteToggle onclick=toggleCollapse('remoteCollapse')>"
  "<span data-i18n=remoteControl>Remote Control</span></button>"
  "<div id=remoteCollapse><div>"
  "<div id=status>...</div>"
  "<div><button onclick=cmd('left')>&larr;</button>"
  "<button onclick=cmd('enter') data-i18n=ok>OK</button>"
  "<button onclick=cmd('right')>&rarr;</button></div>"
  "<div>"
  "<button onmousedown=startHold('up') onmouseup=stopHold() onmouseleave=stopHold()"
  " ontouchstart=startHold('up') ontouchend=stopHold()>&uarr;</button>"
  "<button onmousedown=startHold('down') onmouseup=stopHold() onmouseleave=stopHold()"
  " ontouchstart=startHold('down') ontouchend=stopHold()>&darr;</button>"
  "</div>"
  "<div><button onclick=cmd('mute') data-i18n=mute>Mute</button>"
  "<button onclick=cmd('set') data-i18n=source>Source</button>"
  "<button onclick=powerOffClick() data-i18n=power>Power</button></div>"
  // Температуры ламп/напряжение — приходят от Mega по UART (POWER:/TEMP:/VOLT:, см.
  // mega_link.h), пробрасываются через уже существующий /status (renderStatus() ниже), а не
  // отдельным эндпоинтом — эта панель и так опрашивается каждые 1.5с. Пусто, пока Mega ни разу
  // не прислала своё состояние (megaKnown:false) — например сразу после включения ESP32
  "<div id=megaSensors style='margin-top:8px;font-size:.94em;color:#999;text-align:center'></div>"
  "</div></div>"
  "<div id=trackWrap style='margin-top:14px;display:none'>"
  "<img id=trackArt>"
  "<div id=trackArtPlaceholder><span id=trackArtPlaceholderText></span></div>"
  "<div id=trackArtCube><div id=trackArtCubeInner>"
  "<div class='cubeFace f0'><span></span></div><div class='cubeFace f1'><span></span></div>"
  "<div class='cubeFace f2'><span></span></div><div class='cubeFace f3'><span></span></div>"
  "<div class='cubeFace f4'><span></span></div><div class='cubeFace f5'><span></span></div>"
  "</div></div>"
  "<div id=trackArtPyramid><div id=trackArtPyramidInner>"
  "<div class='pyramidFace p0'><span></span></div><div class='pyramidFace p1'><span></span></div>"
  "<div class='pyramidFace p2'><span></span></div><div class='pyramidFace p3'><span></span></div>"
  "</div></div>"
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
  // Классические "skip" иконки (треугольник + вертикальная черта), не просто угловые скобки —
  // тот же приём, что у play/pause ниже: SVG с fill='currentColor', наследует цвет кнопки
  "<button class=playerBtn onclick=playerCmd('prev',this)>"
  "<svg viewBox='0 0 24 24' width='18' height='18' fill='currentColor'>"
  "<rect x='4' y='4' width='3' height='16'></rect>"
  "<path d='M20 4L8 12L20 20Z'></path>"
  "</svg></button>"
  // Иконка внутри отдельного <span> — playerCmd() запоминает/восстанавливает innerHTML ВСЕЙ
  // кнопки на время запроса (btn.innerHTML='&hellip;'), а pollTrack() ниже должна уметь менять
  // именно иконку (play/pause, в зависимости от j.playing), не трогая остальное. Не один и тот
  // же значок на обе стороны — см. комментарий у pollTrack() за тем, почему теперь можно
  // показывать именно текущее состояние, а не нейтральный комбинированный символ
  "<button id=playPauseBtn class=playerBtn onclick=togglePlayPause(this)>"
  "<span id=playPauseIcon>"
  "<svg viewBox='0 0 24 24' width='18' height='18' fill='currentColor'><path d='M5 3L20 12L5 21Z'></path></svg>"
  "</span></button>"
  "<button class=playerBtn onclick=playerCmd('next',this)>"
  "<svg viewBox='0 0 24 24' width='18' height='18' fill='currentColor'>"
  "<path d='M4 4L16 12L4 20Z'></path>"
  "<rect x='17' y='4' width='3' height='16'></rect>"
  "</svg></button>"
  "<div style='margin-top:8px'>"
  "<input id=volSlider type=range min=0 max=100 value=50 style='width:18%;touch-action:none' "
  "oninput='volDragging=true' onchange=setVolume(this.value)>"
  "</div></div>"
  // IP Arylic вручную и смена Wi-Fi — редко нужные настройки, не часть повседневного
  // управления, поэтому спрятаны за той же раскрывашкой, что и Remote Control выше (см.
  // toggleCollapse()/#settingsCollapse в <style>), а не всегда на виду под плеером
  "<button id=settingsToggle onclick=toggleCollapse('settingsCollapse')>"
  "<span data-i18n=settings>Settings</span></button>"
  "<div id=settingsCollapse><div>"
  "<div style='margin-top:14px'>"
  "<span data-i18n=artTheme>Cover theme</span> "
  "<select id=artThemeSelect onchange=applyArtTheme(this.value)>"
  "<option value=circle data-i18n=artThemeCircle>Circle</option>"
  "<option value=square data-i18n=artThemeSquare>Square</option>"
  "<option value=cube data-i18n=artThemeCube>3D cube</option>"
  "<option value=pyramid data-i18n=artThemePyramid>3D pyramid</option>"
  "</select></div>"
  "<div style='margin-top:14px'>"
  "<input id=arylicIp type=text placeholder='IP Arylic вручную' style='padding:8px;border-radius:6px;border:none'>"
  "<button id=arylicApply onclick=applyArylicIp() data-i18n=apply>Применить</button></div>"
  "<div><button onclick=forgetWifi() style='background:#733' data-i18n=changeWifi>Сменить Wi-Fi</button></div>"
  "</div></div>"
  "</div>"
  // Показывается вместо #mainContent, пока выключено (см. applyPowerState()) — большая круглая
  // кнопка по центру экрана, только она и включает систему обратно. inset:0 — та же самая
  // область на весь вьюпорт, что и у #equalizer, просто по центру и поверх (обычный порядок
  // отрисовки — этот div идёт позже в разметке)
  "<div id=powerOffScreen style='display:none;position:fixed;inset:0;align-items:center;justify-content:center'>"
  "<button id=powerOnBtn onclick=cmd('power') "
  "style='width:140px;height:140px;border-radius:50%;font-size:1.05em;line-height:1.3'>"
  "<span id=powerOnLabel data-i18n=powerOn>Power On</span></button>"
  "</div>"
  "</div>"
  // Экран Spotify (поиск/библиотека/плеер) — полноэкранный, ПОДМЕНЯЕТ собой #controlWrap
  // целиком (см. showSpotifyScreen()/showControlScreen() в <script>), а не раскрывашка внутри
  // обычного экрана: по задумке это отдельная "чистая" страница со своей кнопкой "Назад", а
  // не ещё один пункт среди Remote Control/Settings. Вне #controlWrap — воспроизведение идёт
  // через Arylic как Spotify Connect-цель напрямую из облака Spotify, не через Mega, поэтому
  // доступно независимо от того, включена ли Mega (webPoweredOff)
  "<div id=spotifyScreen style='display:none;position:fixed;inset:0;overflow-y:auto;"
  "background-color:#000;background-image:linear-gradient(135deg,#000,#3a3a3a);z-index:7;padding:10px 14px 90px'>"
  "<div style='display:flex;align-items:center;gap:10px;margin-bottom:12px'>"
  "<button data-i18n=spBack onclick=showControlScreen()>&larr; Назад</button>"
  "<span style='font-size:1.15em;color:#8cf'>Spotify</span></div>"
  "<div id=spLoginWrap style='display:none;text-align:center;padding:20px 0'>"
  "<button class=spPrimaryBtn data-i18n=spLoginBtn onclick=spLogin()>Войти через Spotify</button></div>"
  "<div id=spAppWrap style='display:none'>"
  "<div class=spTabs>"
  "<button id=spTabSearchBtn class=active data-i18n=spTabSearch onclick=spSwitchTab('search')>Поиск</button>"
  "<button id=spTabLibBtn data-i18n=spTabLib onclick=spSwitchTab('lib')>Мои треки</button>"
  "<button id=spTabAlbumsBtn data-i18n=spTabAlbums onclick=spSwitchTab('albums')>Мои альбомы</button>"
  "<button id=spTabPlaylistsBtn data-i18n=spTabPlaylists onclick=spSwitchTab('playlists')>Мои плейлисты</button>"
  "<button id=spTabRecentBtn data-i18n=spTabRecent onclick=spSwitchTab('recent')>Недавнее</button>"
  "<button id=spTabTopBtn data-i18n=spTabTop onclick=spSwitchTab('top')>Топ</button>"
  "<button data-i18n=spLogoutBtn onclick=spLogout() style='margin-left:auto;font-size:.8em'>Выйти</button>"
  "</div>"
  "<div id=spSearchTab>"
  "<div class=spSearchBox>"
  // oninput — раньше поле никак не следило за вводом (только клик по "Найти"), результаты
  // от старого запроса зависали на экране, даже когда поле стёрли целиком. Живой поиск по
  // мере набора текста пробовали (debounce) — не понравилось, вернули обратно на явный клик
  // по "Найти"; oninput остался только для очистки при стирании поля (spClearSearchIfEmpty())
  "<input id=spSearchInput type=text placeholder='Найти трек, исполнителя, плейлист' "
  "oninput=spClearSearchIfEmpty()>"
  "<button data-i18n=spSearchBtn onclick=spSearch()>Найти</button></div>"
  "<div id=spSearchResults></div>"
  "</div>"
  "<div id=spLibTab style='display:none'>"
  "<div id=spLibResults class=spRowList></div>"
  "<button id=spLibMoreBtn data-i18n=spShowMore style='display:none;margin-top:6px' onclick=spLoadLibrary(false)>Показать ещё</button>"
  "</div>"
  "<div id=spPlaylistsTab style='display:none'>"
  "<div id=spPlaylistsResults class=spRowList></div>"
  "<button id=spPlaylistsMoreBtn data-i18n=spShowMore style='display:none;margin-top:6px' onclick=spLoadPlaylists(false)>Показать ещё</button>"
  "</div>"
  "<div id=spAlbumsTab style='display:none'>"
  "<div id=spAlbumsResults class=spRowList></div>"
  "<button id=spAlbumsMoreBtn data-i18n=spShowMore style='display:none;margin-top:6px' onclick=spLoadAlbums(false)>Показать ещё</button>"
  "</div>"
  "<div id=spRecentTab style='display:none'>"
  "<div id=spRecentResults class=spRowList></div>"
  "<button id=spRecentMoreBtn data-i18n=spShowMore style='display:none;margin-top:6px' onclick=spLoadRecent(false)>Показать ещё</button>"
  "</div>"
  // Топ (личный, не путать со снесённым top-tracks артиста) — три переключаемых вида в одном
  // контейнере результатов вместо трёх отдельных вкладок (табов и так уже много, см. spTabs)
  "<div id=spTopTab style='display:none'>"
  "<div class=spSubTabs>"
  "<button id=spTopTracksBtn class=active data-i18n=spTracksLabel onclick=\"spSwitchTopType('tracks')\">Треки</button>"
  "<button id=spTopArtistsBtn data-i18n=spArtistsLabel onclick=\"spSwitchTopType('artists')\">Исполнители</button>"
  "<button id=spTopFollowingBtn data-i18n=spFollowingLabel onclick=\"spSwitchTopType('following')\">Подписки</button>"
  "<select id=spTopRangeSelect onchange=spTopRangeChanged(this.value) style='margin-left:auto'>"
  "<option value=short_term data-i18n=spRangeShort>4 недели</option>"
  "<option value=medium_term selected data-i18n=spRangeMedium>6 месяцев</option>"
  "<option value=long_term data-i18n=spRangeLong>Всё время</option>"
  "</select>"
  "</div>"
  "<div id=spTopResults class=spRowList></div>"
  "</div>"
  "<div id=spDeviceTag class=spDeviceTag onclick=spEnsureDevice()>…</div>"
  "</div>"
  // Экран артиста/плейлиста — подменяет #spAppWrap целиком (не раскрывашка поверх результатов
  // поиска), т.к. попросили именно "результаты поиска скрываются, а контент показывается по
  // центру", а не список поверх списка
  "<div id=spDetailView style='display:none'>"
  "<button data-i18n=spBackToSearch onclick=closeDetailView()>&larr; Назад к поиску</button>"
  "<div id=spDetailHeader style='text-align:center;margin:14px 0'></div>"
  "<div id=spDetailList class=spRowList></div>"
  "</div>"
  // Оверлей выбора плейлиста для "+В плейлист" — поверх всего, включая #spDetailView (можно
  // добавлять в плейлист прямо из карточки артиста/альбома, не только из плоских списков)
  "<div id=spPlaylistPicker style='display:none;position:fixed;inset:0;z-index:9;overflow-y:auto;"
  "background-color:#000;background-image:linear-gradient(135deg,#000,#3a3a3a);padding:10px 14px'>"
  "<div style='display:flex;align-items:center;justify-content:space-between;margin-bottom:12px'>"
  "<span style='font-size:1.05em;color:#8cf' data-i18n=spAddToPlaylistTitle>Добавить в плейлист</span>"
  "<button data-i18n=spCancel onclick=closePlaylistPicker()>Отмена</button></div>"
  "<button class=spPrimaryBtn data-i18n=spNewPlaylist style='width:100%;margin-bottom:10px' onclick=spCreatePlaylistAndAdd()>"
  "+ Новый плейлист</button>"
  "<div id=spPlaylistPickerList class=spRowList></div>"
  "</div>"
  // Мини-плеер — внутри #spotifyScreen (не общий с Control): раньше был снаружи и оставался
  // виден поверх обычного экрана управления даже после "Назад" — по задумке весь плеер должен
  // жить только на странице Spotify. position:fixed внутри — тот же самый низ вьюпорта, но
  // теперь скрывается вместе со всем #spotifyScreen через его общий display:none
  "<div id=spPlayerBar style='display:none;z-index:8'>"
  "<div class=spNpRow>"
  "<img id=spNpThumb class=spThumb>"
  "<div class=spNpMeta>"
  "<span id=spNpName class=spName>—</span>"
  "<span id=spNpArtist class=spSub></span>"
  "</div>"
  "<div class=spTransport>"
  "<button id=spShuffleBtn class=spToggleBtn onclick=spToggleShuffle() title='Перемешать'>&#128256;</button>"
  "<button onclick=spPrev()>"
  "<svg viewBox='0 0 24 24' width='18' height='18'><rect x='4' y='4' width='3' height='16'></rect>"
  "<path d='M20 4L8 12L20 20Z'></path></svg></button>"
  "<button onclick=spTogglePlayPause()><span id=spPlayPauseIcon>"
  "<svg viewBox='0 0 24 24' width='20' height='20'><path d='M5 3L20 12L5 21Z'></path></svg>"
  "</span></button>"
  "<button onclick=spNext()>"
  "<svg viewBox='0 0 24 24' width='18' height='18'><path d='M4 4L16 12L4 20Z'></path>"
  "<rect x='17' y='4' width='3' height='16'></rect></svg></button>"
  "<button id=spRepeatBtn class=spToggleBtn onclick=spCycleRepeat() title='Повтор'>&#128257;</button>"
  "</div></div>"
  "<div class=spSeekRow>"
  "<span id=spCur>0:00</span>"
  "<input id=spSeekSlider type=range min=0 max=1000 value=0 "
  "onpointerdown='spSeekDragging=true' onpointerup=spSeekChanged(this.value) onchange=spSeekChanged(this.value)>"
  "<span id=spDur>0:00</span></div>"
  "<div class=spVolRow>"
  "<span style='font-size:.75em;color:#888'>&#128266;</span>"
  "<input id=spVolSlider type=range min=0 max=100 value=50 "
  "onpointerdown='spVolDragging=true' onchange=spVolChanged(this.value)>"
  "<button data-i18n=spQueueBtn onclick=openQueueView() style='font-size:.7em;padding:3px 8px;flex-shrink:0'>Очередь</button>"
  "</div>"
  "</div>"
  "</div>"
  // Подпись — вне #mainContent/#powerOffScreen, видна всегда независимо от состояния питания
  "<div style='text-align:center;font-size:.7em;color:#666;margin:20px 0 8px'>"
  "Designed and Developed by Egor Avksentyev &amp; Sergey Ladnov</div>"
  // Логотип Pathfinder — PNG с прозрачным фоном (чёрный фон исходника заменён на
  // альфа-канал по яркости, обрезан по границам содержимого, ужат до 280px) вместо
  // сплошного чёрного прямоугольника — вписывается в градиент страницы. Встроен как
  // base64 прямо в PROGMEM
  "<div style='text-align:center;margin:4px 0 16px'><img style='width:140px' src='data:image/png;base64,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"
  "AAAASUVORK5CYII='></div>"
  "<script>"
  "function cmd(a){fetch('/cmd?action='+a)}"
  // Mega физически довозит Bass/High/Volume к нулю и держит 3с экран "POWER OFF" ПЕРЕД тем,
  // как реально обесточиться (см. CLAUDE.md/on_off_logic.cpp в репозитории Mega,
  // seekBassHighVolumeToZeroBlocking()+powerOffDevices()) — всё это время цикл на Mega
  // блокирован целиком и не читает UART от ESP32 вообще, так что "Power On", нажатый в это
  // окно, физически не может подействовать раньше, чем Mega освободится (~8с по факту на
  // живом устройстве). Mega ничего не подтверждает обратно (UART в одну сторону, см. README) —
  // это лишь оценка сверху для индикации, не гарантия: если Mega освободится позже, кнопка
  // просто станет активной чуть раньше, чем реально сработает
  "const MEGA_POWEROFF_BUSY_MS=12000;"
  "let megaBusyUntil=0;"
  "function powerOffClick(){megaBusyUntil=Date.now()+MEGA_POWEROFF_BUSY_MS;cmd('power')}"
  "function updatePowerOnBtn(){"
  "let btn=document.getElementById('powerOnBtn');"
  "let remain=megaBusyUntil-Date.now();"
  "if(remain>0){"
  "btn.disabled=true;"
  "document.getElementById('powerOnLabel').innerText=I18N[currentLang].poweringOff+'… '+Math.ceil(remain/1000);"
  "}else{"
  "btn.disabled=false;"
  "document.getElementById('powerOnLabel').innerText=I18N[currentLang].powerOn;"
  "}}"
  "setInterval(updatePowerOnBtn,250);"
  "let holdTimer=null;"
  "function startHold(a){cmd(a);holdTimer=setInterval(()=>cmd(a),150)}"
  "function stopHold(){if(holdTimer){clearInterval(holdTimer);holdTimer=null}}"
  "function toggleCollapse(id){document.getElementById(id).classList.toggle('open')}"
  // Много столбиков на всю высоту, не 5 — иначе не похоже на настоящий эквалайзер. Генерируем
  // через JS, а не перечисляем вручную десятки CSS-правил nth-child(N): top равномерно по
  // всей высоте, длительность/задержка анимации — со случайным разбросом (иначе все столбики
  // скакали бы синхронно, одной волной, а не вразнобой, как у настоящего эквалайзера)
  "(function buildEqualizer(){"
  "let eq=document.getElementById('equalizer');"
  "let count=30;"
  "for(let i=0;i<count;i++){"
  "let bar=document.createElement('div');bar.className='eqBar';"
  "bar.style.top=((i+.5)/count*100)+'%';"
  "bar.style.animationDuration=(.6+Math.random()*.8)+'s';"
  "bar.style.animationDelay=(Math.random()*.7)+'s';"
  "eq.appendChild(bar)}"
  "})();"
  // Единая точка нажатия для всего экрана Spotify — делегирование на #spotifyScreen, а не
  // отдельный onpointerdown/onpointerup на каждой кнопке/строке (их уже под сотню с учётом
  // динамически создаваемых строк списков). closest() находит ближайшего кандидата — кнопку,
  // если тапнули по ней (в т.ч. по вложенному svg/span внутри), иначе .spRow, если тапнули по
  // строке трека/артиста/плейлиста. pointerup/pointercancel — на document, не на самом
  // элементе: палец может соскользнуть за пределы кнопки перед отпусканием, тогда обычный
  // "pointerup на этом же элементе" не сработал бы, и подсветка осталась бы висеть навсегда
  "(function setupSpTapFeedback(){"
  "let pressedEl=null;"
  "document.getElementById('spotifyScreen').addEventListener('pointerdown',function(e){"
  "let el=e.target.closest('#spotifyScreen button,#spotifyScreen .spRow');"
  "if(!el)return;"
  "pressedEl=el;pressedEl.classList.add('pressed')"
  "});"
  "function release(){if(pressedEl){pressedEl.classList.remove('pressed');pressedEl=null}}"
  "document.addEventListener('pointerup',release);"
  "document.addEventListener('pointercancel',release)"
  "})();"
  // Все три языка целиком на клиенте — переключение мгновенное, без похода на сервер и без
  // перезагрузки страницы. НЕ переводятся (сознательно): название трека/исполнителя и имя
  // источника (Spotify/AirPlay/...) — это данные, пришедшие от Arylic, не текст интерфейса
  "const I18N={"
  "ru:{remoteControl:'Пульт',settings:'Настройки',"
  "artTheme:'Тема обложки:',artThemeCircle:'Круг (крутится)',artThemeSquare:'Квадрат (статично)',"
  "artThemeCube:'3D-куб',artThemePyramid:'3D-пирамида',"
  "ok:'OK',mute:'Без звука',source:'Источник',power:'Питание',"
  "powerOn:'Включить',poweringOff:'Выключение',"
  "apply:'Применить',ipPlaceholder:'IP Arylic вручную',changeWifi:'Сменить Wi-Fi',"
  "wifiOk:'Wi-Fi OK',wifiOff:'Wi-Fi отключён',lastCommand:'последняя команда',"
  "invalidIp:'Некорректный IP',"
  "forgetWifiConfirm:'Забыть текущую Wi-Fi сеть и перезагрузиться в режим настройки?',"
  "forgetWifiDone:'Готово. Устройство подняло точку доступа ',"
  "spBack:'\\u2190 Назад',spBackToSearch:'\\u2190 Назад к поиску',spLoginBtn:'Войти через Spotify',"
  "spTabSearch:'Поиск',spTabLib:'Мои треки',spTabAlbums:'Мои альбомы',spTabPlaylists:'Мои плейлисты',"
  "spTabRecent:'Недавнее',spTabTop:'Топ',spLogoutBtn:'Выйти',"
  "spSearchPlaceholder:'Найти трек, исполнителя, плейлист',spSearchBtn:'Найти',spShowMore:'Показать ещё',"
  "spTracksLabel:'Треки',spArtistsLabel:'Исполнители',spFollowingLabel:'Подписки',"
  "spPlaylistsLabelPlural:'Плейлисты',spArtistSingular:'Исполнитель',spPlaylistSingular:'Плейлист',"
  "spRangeShort:'4 недели',spRangeMedium:'6 месяцев',spRangeLong:'Всё время',"
  "spAddToPlaylistTitle:'Добавить в плейлист',spCancel:'Отмена',spNewPlaylist:'+ Новый плейлист',"
  "spQueueBtn:'Очередь',spShuffleTitle:'Перемешать',spRepeatTitle:'Повтор',"
  "spQueueAddTitle:'В очередь',spQueueAddBtn:'+Очередь',spPlaylistAddTitle:'В плейлист',"
  "spPlaylistAddBtn:'+Плейлист',spNotLoggedIn:'не выполнен вход',"
  "spDeviceNotFound:'Устройство не найдено — нажмите, чтобы обновить',"
  "spPlayFailed:'Не удалось запустить: ',spPlayerError:'Ошибка плеера: ',"
  "spSeekFailed:'Не удалось перемотать: ',spSaveFailed:'Не получилось: ',spQueueAdded:'Добавлено',"
  "spQueueFailed:'Не удалось добавить в очередь: ',spLoading:'Загрузка…',"
  "spNoOwnPlaylists:'Нет своих плейлистов — создайте новый',"
  "spAddedToPlaylistPrefix:'Добавлено в «',spAddedToPlaylistSuffix:'»',"
  "spAddToPlaylistFailed:'Не удалось добавить: ',spNewPlaylistPrompt:'Название нового плейлиста:',"
  "spCreatePlaylistFailed:'Не удалось создать плейлист: ',spNowPlaying:'Сейчас играет',spUpNext:'Далее',"
  "spQueueEmpty:'Очередь пуста',spSearching:'Ищем…',spNothingFound:'Ничего не найдено',"
  "spError:'Ошибка: ',spSearchError:'Ошибка поиска: ',spNoData:'Нет данных',"
  "spNotYourPlaylist:'Это не ваш плейлист — Spotify больше не отдаёт чужие треки сторонним приложениям',"
  "spNoSavedTracks:'Нет сохранённых треков',spNoPlaylists:'Нет плейлистов',"
  "spNoSavedAlbums:'Нет сохранённых альбомов',spRecentEmpty:'Пока пусто',spNoFollowing:'Нет подписок',"
  "spNotEnoughData:'Пока недостаточно данных для статистики',spEmpty:'Пусто',spHttpCode:'код ',"
  "megaLampLabel:'Лампа',megaVoltageLabel:'Напряжение',"
  "weather:{clear:'Ясно',cloudy:'Переменная облачность',overcast:'Пасмурно',fog:'Туман',"
  "drizzle:'Морось',freezingDrizzle:'Ледяная морось',rain:'Дождь',heavyRain:'Сильный дождь',"
  "freezingRain:'Ледяной дождь',snow:'Снег',heavySnow:'Сильный снег',snowGrains:'Снежная крупа',"
  "showers:'Ливень',heavyShowers:'Сильный ливень',snowShowers:'Снегопад',thunder:'Гроза',"
  "thunderHail:'Гроза с градом'}},"
  "uk:{remoteControl:'Пульт',settings:'Налаштування',"
  "artTheme:'Тема обкладинки:',artThemeCircle:'Коло (крутиться)',artThemeSquare:'Квадрат (статично)',"
  "artThemeCube:'3D-куб',artThemePyramid:'3D-піраміда',"
  "ok:'OK',mute:'Без звуку',source:'Джерело',power:'Живлення',"
  "powerOn:'Увімкнути',poweringOff:'Вимкнення',"
  "apply:'Застосувати',ipPlaceholder:'IP Arylic вручну',changeWifi:'Змінити Wi-Fi',"
  "wifiOk:'Wi-Fi OK',wifiOff:'Wi-Fi вимкнено',lastCommand:'остання команда',"
  "invalidIp:'Некоректний IP',"
  "forgetWifiConfirm:'Забути поточну мережу Wi-Fi і перезавантажитися в режим налаштування?',"
  "forgetWifiDone:'Готово. Пристрій підняв точку доступу ',"
  "spBack:'\\u2190 Назад',spBackToSearch:'\\u2190 Назад до пошуку',spLoginBtn:'Увійти через Spotify',"
  "spTabSearch:'Пошук',spTabLib:'Мої треки',spTabAlbums:'Мої альбоми',spTabPlaylists:'Мої плейлисти',"
  "spTabRecent:'Нещодавнє',spTabTop:'Топ',spLogoutBtn:'Вийти',"
  "spSearchPlaceholder:'Знайти трек, виконавця, плейлист',spSearchBtn:'Знайти',spShowMore:'Показати ще',"
  "spTracksLabel:'Треки',spArtistsLabel:'Виконавці',spFollowingLabel:'Підписки',"
  "spPlaylistsLabelPlural:'Плейлисти',spArtistSingular:'Виконавець',spPlaylistSingular:'Плейлист',"
  "spRangeShort:'4 тижні',spRangeMedium:'6 місяців',spRangeLong:'Весь час',"
  "spAddToPlaylistTitle:'Додати в плейлист',spCancel:'Скасувати',spNewPlaylist:'+ Новий плейлист',"
  "spQueueBtn:'Черга',spShuffleTitle:'Перемішати',spRepeatTitle:'Повтор',"
  "spQueueAddTitle:'У чергу',spQueueAddBtn:'+Черга',spPlaylistAddTitle:'У плейлист',"
  "spPlaylistAddBtn:'+Плейлист',spNotLoggedIn:'вхід не виконано',"
  "spDeviceNotFound:'Пристрій не знайдено — натисніть, щоб оновити',"
  "spPlayFailed:'Не вдалося запустити: ',spPlayerError:'Помилка плеєра: ',"
  "spSeekFailed:'Не вдалося перемотати: ',spSaveFailed:'Не вдалося: ',spQueueAdded:'Додано',"
  "spQueueFailed:'Не вдалося додати в чергу: ',spLoading:'Завантаження…',"
  "spNoOwnPlaylists:'Немає власних плейлистів — створіть новий',"
  "spAddedToPlaylistPrefix:'Додано до «',spAddedToPlaylistSuffix:'»',"
  "spAddToPlaylistFailed:'Не вдалося додати: ',spNewPlaylistPrompt:'Назва нового плейлиста:',"
  "spCreatePlaylistFailed:'Не вдалося створити плейлист: ',spNowPlaying:'Зараз грає',spUpNext:'Далі',"
  "spQueueEmpty:'Черга порожня',spSearching:'Шукаємо…',spNothingFound:'Нічого не знайдено',"
  "spError:'Помилка: ',spSearchError:'Помилка пошуку: ',spNoData:'Немає даних',"
  "spNotYourPlaylist:'Це не ваш плейлист — Spotify більше не віддає чужі треки стороннім застосункам',"
  "spNoSavedTracks:'Немає збережених треків',spNoPlaylists:'Немає плейлистів',"
  "spNoSavedAlbums:'Немає збережених альбомів',spRecentEmpty:'Поки що порожньо',spNoFollowing:'Немає підписок',"
  "spNotEnoughData:'Поки що недостатньо даних для статистики',spEmpty:'Порожньо',spHttpCode:'код ',"
  "megaLampLabel:'Лампа',megaVoltageLabel:'Напруга',"
  "weather:{clear:'Ясно',cloudy:'Мінлива хмарність',overcast:'Похмуро',fog:'Туман',"
  "drizzle:'Мряка',freezingDrizzle:'Крижана мряка',rain:'Дощ',heavyRain:'Сильний дощ',"
  "freezingRain:'Крижаний дощ',snow:'Сніг',heavySnow:'Сильний сніг',snowGrains:'Снігова крупа',"
  "showers:'Злива',heavyShowers:'Сильна злива',snowShowers:'Снігопад',thunder:'Гроза',"
  "thunderHail:'Гроза з градом'}},"
  "ro:{remoteControl:'Telecomandă',settings:'Setări',"
  "artTheme:'Tema copertei:',artThemeCircle:'Cerc (se rotește)',artThemeSquare:'Pătrat (static)',"
  "artThemeCube:'Cub 3D',artThemePyramid:'Piramidă 3D',"
  "ok:'OK',mute:'Fără sunet',source:'Sursă',power:'Pornire',"
  "powerOn:'Pornește',poweringOff:'Se oprește',"
  "apply:'Aplică',ipPlaceholder:'IP Arylic manual',changeWifi:'Schimbă Wi-Fi',"
  "wifiOk:'Wi-Fi OK',wifiOff:'Wi-Fi deconectat',lastCommand:'ultima comandă',"
  "invalidIp:'IP invalid',"
  "forgetWifiConfirm:'Uiți rețeaua Wi-Fi curentă și repornești în modul de configurare?',"
  "forgetWifiDone:'Gata. Dispozitivul a pornit punctul de acces ',"
  "spBack:'\\u2190 Înapoi',spBackToSearch:'\\u2190 Înapoi la căutare',spLoginBtn:'Conectare cu Spotify',"
  "spTabSearch:'Căutare',spTabLib:'Piesele mele',spTabAlbums:'Albumele mele',"
  "spTabPlaylists:'Playlisturile mele',spTabRecent:'Recente',spTabTop:'Top',spLogoutBtn:'Deconectare',"
  "spSearchPlaceholder:'Caută piesă, artist, playlist',spSearchBtn:'Caută',spShowMore:'Arată mai multe',"
  "spTracksLabel:'Piese',spArtistsLabel:'Artiști',spFollowingLabel:'Urmăriți',"
  "spPlaylistsLabelPlural:'Playlisturi',spArtistSingular:'Artist',spPlaylistSingular:'Playlist',"
  "spRangeShort:'4 săptămâni',spRangeMedium:'6 luni',spRangeLong:'Tot timpul',"
  "spAddToPlaylistTitle:'Adaugă în playlist',spCancel:'Anulează',spNewPlaylist:'+ Playlist nou',"
  "spQueueBtn:'Coadă',spShuffleTitle:'Amestecă',spRepeatTitle:'Repetă',"
  "spQueueAddTitle:'În coadă',spQueueAddBtn:'+Coadă',spPlaylistAddTitle:'În playlist',"
  "spPlaylistAddBtn:'+Playlist',spNotLoggedIn:'neautentificat',"
  "spDeviceNotFound:'Dispozitiv negăsit — apasă pentru a reîmprospăta',"
  "spPlayFailed:'Nu s-a putut porni: ',spPlayerError:'Eroare player: ',"
  "spSeekFailed:'Nu s-a putut derula: ',spSaveFailed:'Nu a reușit: ',spQueueAdded:'Adăugat',"
  "spQueueFailed:'Nu s-a putut adăuga în coadă: ',spLoading:'Se încarcă…',"
  "spNoOwnPlaylists:'Nu ai playlisturi proprii — creează unul nou',"
  "spAddedToPlaylistPrefix:'Adăugat în „',spAddedToPlaylistSuffix:'”',"
  "spAddToPlaylistFailed:'Nu s-a putut adăuga: ',spNewPlaylistPrompt:'Numele noului playlist:',"
  "spCreatePlaylistFailed:'Nu s-a putut crea playlistul: ',spNowPlaying:'Se redă acum',spUpNext:'Următoarele',"
  "spQueueEmpty:'Coada este goală',spSearching:'Se caută…',spNothingFound:'Nimic găsit',"
  "spError:'Eroare: ',spSearchError:'Eroare de căutare: ',spNoData:'Fără date',"
  "spNotYourPlaylist:'Acesta nu este playlistul tău — Spotify nu mai oferă piesele altora aplicațiilor terțe',"
  "spNoSavedTracks:'Nicio piesă salvată',spNoPlaylists:'Niciun playlist',"
  "spNoSavedAlbums:'Niciun album salvat',spRecentEmpty:'Deocamdată gol',spNoFollowing:'Nu urmărești pe nimeni',"
  "spNotEnoughData:'Încă nu sunt suficiente date pentru statistici',spEmpty:'Gol',spHttpCode:'cod ',"
  "megaLampLabel:'Lampă',megaVoltageLabel:'Tensiune',"
  "weather:{clear:'Senin',cloudy:'Parțial noros',overcast:'Înnorat',fog:'Ceață',"
  "drizzle:'Burniță',freezingDrizzle:'Burniță înghețată',rain:'Ploaie',heavyRain:'Ploaie puternică',"
  "freezingRain:'Ploaie înghețată',snow:'Ninsoare',heavySnow:'Ninsoare puternică',"
  "snowGrains:'Măzăriche de zăpadă',showers:'Averse',heavyShowers:'Averse puternice',"
  "snowShowers:'Ninsoare abundentă',thunder:'Furtună',thunderHail:'Furtună cu grindină'}},"
  "en:{remoteControl:'Remote Control',settings:'Settings',"
  "artTheme:'Cover theme:',artThemeCircle:'Circle (spinning)',artThemeSquare:'Square (static)',"
  "artThemeCube:'3D cube',artThemePyramid:'3D pyramid',"
  "ok:'OK',mute:'Mute',source:'Source',power:'Power',"
  "powerOn:'Power On',poweringOff:'Powering off',"
  "apply:'Apply',ipPlaceholder:'Arylic IP manually',changeWifi:'Change Wi-Fi',"
  "wifiOk:'Wi-Fi OK',wifiOff:'Wi-Fi disconnected',lastCommand:'last command',"
  "invalidIp:'Invalid IP',"
  "forgetWifiConfirm:'Forget the current Wi-Fi network and reboot into setup mode?',"
  "forgetWifiDone:'Done. The device brought up the access point ',"
  "spBack:'\\u2190 Back',spBackToSearch:'\\u2190 Back to search',spLoginBtn:'Log in with Spotify',"
  "spTabSearch:'Search',spTabLib:'My tracks',spTabAlbums:'My albums',spTabPlaylists:'My playlists',"
  "spTabRecent:'Recent',spTabTop:'Top',spLogoutBtn:'Log out',"
  "spSearchPlaceholder:'Find a track, artist, playlist',spSearchBtn:'Search',spShowMore:'Show more',"
  "spTracksLabel:'Tracks',spArtistsLabel:'Artists',spFollowingLabel:'Following',"
  "spPlaylistsLabelPlural:'Playlists',spArtistSingular:'Artist',spPlaylistSingular:'Playlist',"
  "spRangeShort:'4 weeks',spRangeMedium:'6 months',spRangeLong:'All time',"
  "spAddToPlaylistTitle:'Add to playlist',spCancel:'Cancel',spNewPlaylist:'+ New playlist',"
  "spQueueBtn:'Queue',spShuffleTitle:'Shuffle',spRepeatTitle:'Repeat',"
  "spQueueAddTitle:'Add to queue',spQueueAddBtn:'+Queue',spPlaylistAddTitle:'Add to playlist',"
  "spPlaylistAddBtn:'+Playlist',spNotLoggedIn:'not logged in',"
  "spDeviceNotFound:'No device found — tap to refresh',"
  "spPlayFailed:'Could not start playback: ',spPlayerError:'Player error: ',"
  "spSeekFailed:'Could not seek: ',spSaveFailed:'Failed: ',spQueueAdded:'Added',"
  "spQueueFailed:'Could not add to queue: ',spLoading:'Loading…',"
  "spNoOwnPlaylists:'You have no playlists of your own — create one',"
  "spAddedToPlaylistPrefix:'Added to \\u201c',spAddedToPlaylistSuffix:'\\u201d',"
  "spAddToPlaylistFailed:'Could not add: ',spNewPlaylistPrompt:'New playlist name:',"
  "spCreatePlaylistFailed:'Could not create the playlist: ',spNowPlaying:'Now playing',spUpNext:'Up next',"
  "spQueueEmpty:'Queue is empty',spSearching:'Searching…',spNothingFound:'Nothing found',"
  "spError:'Error: ',spSearchError:'Search error: ',spNoData:'No data',"
  "spNotYourPlaylist:'This is not your playlist — Spotify no longer gives other people\\'s tracks to third-party apps',"
  "spNoSavedTracks:'No saved tracks',spNoPlaylists:'No playlists',"
  "spNoSavedAlbums:'No saved albums',spRecentEmpty:'Nothing yet',spNoFollowing:'Not following anyone',"
  "spNotEnoughData:'Not enough listening data yet',spEmpty:'Empty',spHttpCode:'code ',"
  "megaLampLabel:'Lamp',megaVoltageLabel:'Voltage',"
  "weather:{clear:'Clear',cloudy:'Partly cloudy',overcast:'Overcast',fog:'Fog',"
  "drizzle:'Drizzle',freezingDrizzle:'Freezing drizzle',rain:'Rain',heavyRain:'Heavy rain',"
  "freezingRain:'Freezing rain',snow:'Snow',heavySnow:'Heavy snow',snowGrains:'Snow grains',"
  "showers:'Showers',heavyShowers:'Heavy showers',snowShowers:'Snow showers',thunder:'Thunderstorm',"
  "thunderHail:'Thunderstorm with hail'}}"
  "};"
  "let currentLang='uk';"
  "let lastStatus=null;"
  // Объявлены здесь, ДО восстановления сохранённого языка ниже (та вызывает applyLanguage()
  // -> renderWeather() сразу же, синхронно, при загрузке скрипта) — если бы эти let остались
  // на своём "логическом" месте рядом с loadWeather() (ниже по файлу), скрипт падал бы на
  // старте с ReferenceError (temporal dead zone: переменная объявлена, но её строка кода еще
  // не выполнилась к моменту первого обращения)
  "let lastWeatherCode=null,lastWeatherTemp=null,lastWeatherCity=null;"
  "function renderStatus(){"
  "if(!lastStatus)return;"
  "let t=I18N[currentLang];"
  "let s=lastStatus.wifi?t.wifiOk:t.wifiOff;"
  "if(lastStatus.lastCmd)s+=' | '+t.lastCommand+': '+lastStatus.lastCmd;"
  "document.getElementById('status').innerText=s;"
  "applyPowerState(lastStatus.poweredOff);"
  // Температуры/напряжение — реально пришли от Mega по UART (см. handleStatus() на ESP32),
  // не просто последняя отправленная команда. megaKnown==false — Mega ещё ни разу не
  // присылала своё состояние (например ESP32 только что перезагрузился) — тогда пусто,
  // не рисуем нули/прочерки, которые выглядели бы как настоящие показания
  "let ms=document.getElementById('megaSensors');"
  "if(lastStatus.megaKnown){"
  "let parts=lastStatus.temps.map((v,i)=>t.megaLampLabel+' '+(i+1)+': '+"
  "(v<=-100?'--':v)+'\\u00b0C');"
  "if(lastStatus.voltageKnown)parts.push(t.megaVoltageLabel+': '+lastStatus.voltage+'V');"
  "ms.innerText=parts.join('  \\u00b7  ')"
  "}else{ms.innerText=''}"
  "}"
  // Состояние "выключено" теперь отражает настоящее состояние Mega (POWER: по UART, см.
  // applyWebPowerState() в web_control.cpp) — раньше (до 2026-09-21) было чисто оптимистичным
  // отражением последнего клика на ЭТОЙ веб-странице, Mega ничего не отправляла назад вообще.
  // webPoweredOff всё ещё живёт на ESP32 (не только в JS вкладки) — переживает перезагрузку
  // страницы: свежий /status сразу вернёт то, что было, и покажет нужный экран без лишнего
  // мигания основным интерфейсом на долю секунды
  "let lastPoweredOff=null;"
  "function applyPowerState(off){"
  "if(off===lastPoweredOff)return;"
  "lastPoweredOff=off;"
  "document.getElementById('mainContent').style.display=off?'none':'block';"
  "document.getElementById('powerOffScreen').style.display=off?'flex':'none';"
  "document.getElementById('equalizer').style.display=off?'none':'block'}"
  // Один общий обработчик на все элементы с data-i18n (innerText) — не нужно перечислять их
  // поштучно и держать список синхронным с разметкой. ipPlaceholder — отдельно, у input это
  // атрибут placeholder, не innerText
  "function applyLanguage(lang){"
  "let t=I18N[lang];"
  "if(!t)return;"
  "currentLang=lang;"
  "document.querySelectorAll('[data-i18n]').forEach(el=>{el.innerText=t[el.getAttribute('data-i18n')]});"
  "document.getElementById('arylicIp').placeholder=t.ipPlaceholder;"
  "document.getElementById('spSearchInput').placeholder=t.spSearchPlaceholder;"
  "document.getElementById('spShuffleBtn').title=t.spShuffleTitle;"
  "document.getElementById('spRepeatBtn').title=t.spRepeatTitle;"
  "try{localStorage.setItem('lang',lang)}catch(e){}"
  "renderStatus();"
  "renderWeather()}"
  // Восстановление сохранённого языка — сразу при загрузке страницы, до первого опроса
  // /status и /track (та же причина, что у remoteControl-кнопки — заголовок ниже уже должен
  // быть на нужном языке с первого кадра, не мигать русским текстом на долю секунды)
  "(function(){let saved='uk';"
  "try{saved=localStorage.getItem('lang')||'uk'}catch(e){}"
  "if(!I18N[saved])saved='uk';"
  "document.getElementById('langSelect').value=saved;"
  "applyLanguage(saved)})();"
  "function poll(){fetch('/status').then(r=>r.json()).then(j=>{lastStatus=j;renderStatus()})}"
  "setInterval(poll,1500);poll();"
  "function applyArylicIp(){"
  "let v=document.getElementById('arylicIp').value;"
  "fetch('/arylic-ip?ip='+encodeURIComponent(v),{method:'POST'})"
  ".then(r=>{if(!r.ok)alert(I18N[currentLang].invalidIp)})}"
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
  // См. togglePlayPause() ниже за тем, зачем нужна эта пара (null — не ждём подтверждения)
  "let playPauseExpected=null,playPauseExpectedSince=0;"
  // Тема обложки — per-viewer выбор, живёт в localStorage (тот же приём, что у lang выше),
  // не на сервере: это чисто оформление этой конкретной вкладки, а не состояние Mega/Arylic.
  // lastArt*/renderArt() ниже — чтобы применить новую тему СРАЗУ по выбору в дропдауне, не
  // дожидаясь следующего /track-опроса (та же идея, что у togglePlayPause() выше)
  "let artTheme='circle';"
  "try{artTheme=localStorage.getItem('artTheme')||'circle'}catch(e){}"
  "document.getElementById('artThemeSelect').value=artTheme;"
  "let lastArtUrl='',lastArtSource='',artEverRendered=false;"
  "function applyArtTheme(t){"
  "artTheme=t;"
  "try{localStorage.setItem('artTheme',t)}catch(e){}"
  "renderArt(lastArtUrl,lastArtSource)}"
  // Какой из четырёх элементов сейчас реально виден — ровно один, у остальных display:none
  // (см. applyNow() ниже). Нужна для слайд-перехода: не важно, ЧТО именно показано (тема не
  // меняется здесь), важно что именно эту DOM-ноду анимировать
  "function currentArtEl(){"
  "let ids=['trackArt','trackArtPlaceholder','trackArtCube','trackArtPyramid'];"
  "for(let i=0;i<ids.length;i++){"
  "let el=document.getElementById(ids[i]);"
  "if(el.style.display!=='none')return el"
  "}"
  "return null"
  "}"
  // Четыре темы: круг/квадрат — просто переключают .themeCircle на #trackArt/#trackArtPlaceholder
  // (см. <style> выше), куб/пирамида — отдельные элементы (#trackArtCube/#trackArtPyramid) с
  // несколькими гранями, background-image которых выставляется тут же, общим querySelectorAll на
  // весь набор граней сразу. Заглушка без обложки (нет источника с картинкой, см.
  // #trackArtPlaceholder в <style>) для этих двух тем использует её же круглый вид — куб/пирамида
  // из текста без изображения на гранях выглядели бы бессмысленно.
  //
  // changed — настоящая смена трека (не первый показ, не повторный вызов с тем же артом/
  // источником при переключении темы через applyArtTheme()) — только тогда едет в сторону,
  // как карусель: текущая фигура уезжает влево, затем на её месте (или на другом элементе,
  // если заодно поменялась обложка<->заглушка) уже с новым содержимым въезжает справа
  "function renderArt(artUrl,source){"
  "let changed=artEverRendered&&((artUrl||'')!==lastArtUrl||(source||'')!==lastArtSource);"
  "lastArtUrl=artUrl||'';lastArtSource=source||'';artEverRendered=true;"
  "function applyNow(){"
  "let art=document.getElementById('trackArt');"
  "let placeholder=document.getElementById('trackArtPlaceholder');"
  "let cube=document.getElementById('trackArtCube');"
  "let pyramid=document.getElementById('trackArtPyramid');"
  // Сброс transform/opacity у ВСЕХ четырёх при каждом обновлении — иначе элемент, который
  // когда-то уехал влево слайдом, но сейчас скрыт (display:none), при следующем показе (без
  // слайда, просто повторный applyNow()) вылез бы с этой же "залипшей" смещённой позиции
  "art.style.transition=placeholder.style.transition=cube.style.transition=pyramid.style.transition='none';"
  "art.style.transform=placeholder.style.transform=cube.style.transform=pyramid.style.transform='translateX(0)';"
  "art.style.opacity=placeholder.style.opacity=cube.style.opacity=pyramid.style.opacity='1';"
  "art.style.display='none';placeholder.style.display='none';"
  "cube.style.display='none';pyramid.style.display='none';"
  "if(source&&(artTheme==='cube'||artTheme==='pyramid')){"
  "let el=artTheme==='cube'?cube:pyramid;"
  "let faceSel=artTheme==='cube'?'#trackArtCubeInner .cubeFace':'#trackArtPyramidInner .pyramidFace';"
  "el.style.display='block';"
  "document.querySelectorAll(faceSel).forEach(f=>{"
  "f.style.backgroundImage=artUrl?\"url('\"+artUrl+\"')\":'linear-gradient(135deg,#000,#3a3a3a)';"
  "f.querySelector('span').innerText=artUrl?'':source"
  "})"
  "}else if(artUrl){"
  "if(art.src!==artUrl)art.src=artUrl;"
  "art.classList.toggle('themeCircle',artTheme==='circle');"
  "art.style.display='block'"
  "}else if(source){"
  "placeholder.classList.toggle('themeCircle',artTheme!=='square');"
  "placeholder.style.display='flex';"
  "document.getElementById('trackArtPlaceholderText').innerText=source"
  "}"
  "updatePlayVisuals(trackPlayingNow)"
  "}"
  "if(changed){"
  "let outEl=currentArtEl();"
  "if(outEl){"
  "outEl.style.transition='transform .4s ease-in,opacity .4s ease-in';"
  "outEl.style.transform='translateX(-60px)';"
  "outEl.style.opacity='0'"
  "}"
  "setTimeout(()=>{"
  "applyNow();"
  "let inEl=currentArtEl();"
  "if(inEl){"
  "inEl.style.transition='none';"
  "inEl.style.transform='translateX(60px)';"
  "inEl.style.opacity='0';"
  "inEl.offsetHeight;"
  "inEl.style.transition='transform .4s ease-out,opacity .4s ease-out';"
  "inEl.style.transform='translateX(0)';"
  "inEl.style.opacity='1'"
  "}"
  "},380)"
  "}else{"
  "applyNow()"
  "}"
  "}"
  "function fmtTime(ms){let s=Math.max(0,Math.floor(ms/1000));let m=Math.floor(s/60);s=s%60;"
  "return m+':'+(s<10?'0':'')+s}"
  // Play (треугольник) — показывается, когда СЕЙЧАС не играет (нажатие возобновит); Pause (два
  // прямоугольника) — когда играет (нажатие поставит на паузу). j.playing — это trackPlaying на
  // ESP32 (arylicTrackIsPlaying()), которое для AirPlay идёт уже ЧЕРЕЗ оптимистичный оверрайд
  // (arylicNotifyOnepausePressed(), см. arylic_metadata.h) — то есть верно отражает результат
  // именно ЭТОЙ кнопки, хоть и не поймает паузу, поставленную где-то ещё (см. память проекта:
  // project_arylic_airplay_no_metadata — так и остаётся, это не баг иконки, а предел того, что
  // вообще можно узнать про AirPlay)
  "const PLAY_ICON=`<svg viewBox='0 0 24 24' width='18' height='18' fill='currentColor'>"
  "<path d='M5 3L20 12L5 21Z'></path></svg>`;"
  "const PAUSE_ICON=`<svg viewBox='0 0 24 24' width='18' height='18' fill='currentColor'>"
  "<rect x='5' y='3' width='5' height='18'></rect><rect x='14' y='3' width='5' height='18'></rect></svg>`;"
  // Общая для двух источников правды: реального ответа /track (pollTrack() ниже, раз в 500мс)
  // И самого нажатия на кнопку play/pause (см. togglePlayPause() — реагирует МГНОВЕННО на
  // клик, не дожидаясь следующего опроса) — иконка/пластинка/эквалайзер обновляются одним и
  // тем же кодом в обоих случаях, не дублируются
  "function updatePlayVisuals(playing){"
  "document.getElementById('playPauseIcon').innerHTML=playing?PAUSE_ICON:PLAY_ICON;"
  "let state=playing?'running':'paused';"
  "document.getElementById('trackArt').style.animationPlayState=state;"
  "document.getElementById('trackArtPlaceholder').style.animationPlayState=state;"
  "document.getElementById('trackArtCubeInner').style.animationPlayState=state;"
  "document.getElementById('trackArtPyramidInner').style.animationPlayState=state;"
  "document.getElementById('equalizer').classList.toggle('playing',playing)}"
  // playPauseExpected/-Since — pollTrack() ниже игнорирует любой ответ /track, не совпадающий
  // с этим ожиданием, пока оно не подтвердится (или не протухнет по таймауту). Раньше вместо
  // этого был просто !playerCmdBusy — недостаточно: playerCmdBusy снимается сразу, как только
  // САМА HTTPS-команда onepause долетела до Arylic, но фоновый опрос Arylic (arylic_metadata.cpp,
  // независимая задача на ESP32) обновляет закэшированное trackPlaying СВОИМ отдельным циклом —
  // для источников без оптимистичного оверрайда (не AirPlay, см. arylicNotifyOnepausePressed())
  // между этими двумя моментами ещё оставалось окно, где /track успевал вернуть СТАРОЕ значение
  // уже после снятия playerCmdBusy — иконка прыгала на него и только следующим опросом
  // возвращалась в верное состояние (треугольник -> пауза -> треугольник)
  "function togglePlayPause(btn){"
  "trackPlayingNow=!trackPlayingNow;"
  "playPauseExpected=trackPlayingNow;playPauseExpectedSince=Date.now();"
  "updatePlayVisuals(trackPlayingNow);"
  "playerCmd('onepause',btn)}"
  // hasTrack — трек ли играет ПРЯМО СЕЙЧАС, или он просто на паузе (j.text/j.art остаются
  // заполнены сервером и на паузе, см. arylic_metadata.cpp, ветка "не играет" — очищаются
  // только когда Arylic реально пропал из сети, не на обычной паузе кнопкой Play/Pause).
  // Поэтому обложку/заголовок/источник показываем по наличию данных, а не по j.playing —
  // иначе они бы гасли на каждую паузу, что и так видно на паузе (не нужно)
  // См. playPauseExpected/togglePlayPause() выше — та же гонка, что у volDragging/
  // trackSeekDragging: игнорируем любой ответ, не совпадающий с ожиданием, пока не совпадёт
  // (или не протухнет по таймауту, на случай если сама команда не применилась)
  "function pollTrack(){fetch('/track').then(r=>r.json()).then(j=>{"
  "trackLen=j.len;"
  "if(playPauseExpected===null||j.playing===playPauseExpected||Date.now()-playPauseExpectedSince>4000){"
  "trackPlayingNow=j.playing;updatePlayVisuals(j.playing);playPauseExpected=null}"
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
  // распарсились ли title/artist (AirPlay их не отдаёт вообще, но источник знать можно).
  // Раньше без обложки (AirPlay) эта строка была единственным видимым текстом и показывалась
  // крупным/жирным — теперь эту роль вместо неё играет #trackArtPlaceholder ниже (тот же
  // текст, крутится в кружке вместо обложки), так что здесь всегда обычный маленький размер
  "let src=document.getElementById('trackSource');"
  "if(j.source){src.innerText=j.source;src.style.display='block'}"
  "else{src.style.display='none'}"
  // Позиция трека для AirPlay не двигается вообще (устройство её не отдаёт ни в одном
  // известном API, проверено live — см. arylic_metadata.h) — полоска бы просто застыла на
  // месте и вводила в заблуждение, поэтому для этого источника прячем её целиком
  "document.getElementById('trackProgress').style.display=(j.source==='AirPlay')?'none':'block';"
  // Обложка отдаётся ссылкой на CDN сервиса-источника, не байтами — сам img её и грузит.
  // Пусто, если сервис её не отдаёт (например AirPlay/Apple Music, см. arylic_metadata.h) —
  // см. renderArt() за тем, что показывается вместо неё и как выбирается тема (круг/квадрат/куб)
  "renderArt(j.art,j.source);"
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
  // через Open-Meteo (тоже бесплатный, без ключа, CORS *), коды по таблице WMO.
  // Описание — по общему ключу (не прямо по коду WMO), сам текст на 3 языках — в I18N[..].weather
  // выше; так каждый перевод пишется один раз, а не по разу на каждый числовой код WMO
  "const WEATHER_LABEL_BY_CODE={0:'clear',1:'clear',2:'cloudy',3:'overcast',"
  "45:'fog',48:'fog',51:'drizzle',53:'drizzle',"
  "55:'drizzle',56:'freezingDrizzle',57:'freezingDrizzle',61:'rain',"
  "63:'rain',65:'heavyRain',66:'freezingRain',67:'freezingRain',"
  "71:'snow',73:'snow',75:'heavySnow',77:'snowGrains',"
  "80:'showers',81:'showers',82:'heavyShowers',85:'snowShowers',"
  "86:'snowShowers',95:'thunder',96:'thunderHail',99:'thunderHail'};"
  // Иконка по тому же коду WMO — отдельная таблица, не завязана на текст описания (и потому не
  // нуждается в переводе — это просто эмодзи-картинка)
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
  // Название города — как его вернул сервис геолокации, не переводим (имя собственное).
  // Код/температуру/город кэшируем в lastWeather* (объявлены выше, см. комментарий там) —
  // при смене языка (applyLanguage()) нужно перерисовать текст БЕЗ нового опроса погоды,
  // иначе после переключения языка старое описание висело бы ещё до 30 минут (следующий
  // loadWeather())
  "function loadWeather(){fetchLocation().then(loc=>"
  "fetch('https://api.open-meteo.com/v1/forecast?latitude='+loc.lat+'&longitude='+loc.lon"
  "+'&current_weather=true').then(r=>r.json()).then(w=>{"
  "let cw=w.current_weather;"
  "lastWeatherCode=cw.weathercode;lastWeatherTemp=Math.round(cw.temperature);lastWeatherCity=loc.city;"
  "renderWeather()}))"
  ".catch(()=>{})}"
  "function renderWeather(){"
  "if(lastWeatherCode===null)return;"
  "document.getElementById('weatherIcon').innerText=WEATHER_ICONS[lastWeatherCode]||'';"
  "let key=WEATHER_LABEL_BY_CODE[lastWeatherCode];"
  "let desc=(I18N[currentLang].weather[key])||'';"
  "document.getElementById('weatherText').innerText=lastWeatherCity+': '+lastWeatherTemp+'°C, '+desc}"
  // Раз в 30 минут — погода не меняется поминутно, незачем дёргать сторонние сервисы чаще
  "loadWeather();setInterval(loadWeather,1800000);"
  "function forgetWifi(){if(confirm(I18N[currentLang].forgetWifiConfirm)){"
  "fetch('/wifi-forget',{method:'POST'})"
  ".then(()=>alert(I18N[currentLang].forgetWifiDone+'" WIFI_PROVISION_AP_SSID "'))}}"
  // ---- Spotify: логин (PKCE через GitHub Pages, см. docs/spotify/), поиск, "Мои треки",
  // мини-плеер через Arylic как Spotify Connect-цель. Всё общение — напрямую из этого JS в
  // api.spotify.com/accounts.spotify.com, ESP32 тут только раздаёт статику, ничего не
  // проксирует и ничего о Spotify не знает — см. CLAUDE.md/README за архитектурой целиком.
  // Client ID — не секрет при Authorization Code+PKCE (Client Secret тут вообще не участвует),
  // нормально держать в открытом JS; то же значение должно быть вписано в docs/spotify/
  // login.html и callback.html — все три места ссылаются на одно и то же Spotify-приложение
  "const SP_CLIENT_ID='1a0298a844e24d6d8ab31c9342760d42';"
  "const SP_LOGIN_URL='https://egor-avksentyev.github.io/esp_32_ethernet_project/spotify/login.html';"
  "function spTok(){try{return JSON.parse(localStorage.getItem('spTok')||'null')}catch(e){return null}}"
  "function spSaveTok(t){try{localStorage.setItem('spTok',JSON.stringify(t))}catch(e){}}"
  "function spLogout(){"
  "try{localStorage.removeItem('spTok')}catch(e){}"
  "try{localStorage.removeItem('spDeviceId')}catch(e){}"
  "spDeviceId=null;spLastPlayback=null;"
  "if(spPollTimer){clearInterval(spPollTimer);spPollTimer=null}"
  "document.getElementById('spPlayerBar').style.display='none';"
  "renderSpotifyAuth()}"
  "function spLogin(){location.href=SP_LOGIN_URL+'?return='+encodeURIComponent(location.origin+location.pathname)}"
  // Возврат из callback.html (см. docs/spotify/callback.html) — токен во фрагменте адреса,
  // не в query, поэтому сервер ESP32 его никогда не видит (см. пояснение там же). Once —
  // сразу вычищаем фрагмент из адресной строки, чтобы токен не остался виден и не
  // переобработался повторно при обновлении страницы
  "async function spRefresh(){"
  "let t=spTok();if(!t||!t.refresh)return null;"
  "let body=new URLSearchParams({client_id:SP_CLIENT_ID,grant_type:'refresh_token',refresh_token:t.refresh});"
  "let r=await fetch('https://accounts.spotify.com/api/token',"
  "{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});"
  "let d=await r.json();if(!r.ok)return null;"
  "let nt={access:d.access_token,refresh:d.refresh_token||t.refresh,exp:Date.now()+d.expires_in*1000};"
  "spSaveTok(nt);return nt.access}"
  "async function spToken(){"
  "let t=spTok();if(!t)return null;"
  "if(Date.now()<t.exp-30000)return t.access;"
  "return await spRefresh()}"
  "async function spApi(path,opts){"
  "opts=opts||{};"
  "let tok=await spToken();if(!tok){spLogout();throw new Error(I18N[currentLang].spNotLoggedIn)}"
  // Content-Type:application/json ТОЛЬКО когда реально есть тело (play с uris/context) — на
  // next/previous/pause/resume тело не шлём вообще, и этот заголовок без тела иногда заставляет
  // сторону Spotify (или CDN перед ней) вернуть не-JSON страницу вместо обычного 204/JSON —
  // именно так на практике проявлялась ошибка "JSON parse error" на кнопках play/pause/next/prev
  "let headers=Object.assign({Authorization:'Bearer '+tok},opts.headers||{});"
  "if(opts.body)headers['Content-Type']='application/json';"
  "let r=await fetch('https://api.spotify.com/v1'+path,Object.assign({},opts,{headers}));"
  "if(r.status===204)return null;"
  "let text=await r.text();"
  // Иногда Spotify (или CDN перед ней) на команды плеера отвечает 200 с телом, которое не
  // разбирается как JSON (не документировано, но воспроизводится живьём на play/pause/next/
  // prev) — при этом сама команда реально выполняется. Раз ни один из вызовов плеера не
  // использует возвращаемые данные, не разобравшееся тело не должно считаться ошибкой, ЕСЛИ
  // сам HTTP-статус успешный (r.ok) — падаем только когда статус ошибочный
  "let data=null;"
  "if(text){try{data=JSON.parse(text)}catch(e){data=null}}"
  "if(!r.ok){let err=new Error((data&&data.error&&data.error.message)||"
  "(I18N[currentLang].spHttpCode+r.status+(text?': '+text.slice(0,120):'')));err.status=r.status;throw err}"
  "return data}"
  "function spQS(parts){parts=parts.filter(Boolean);return parts.length?'?'+parts.join('&'):''}"
  "function spDevParam(){return spDeviceId?('device_id='+encodeURIComponent(spDeviceId)):''}"
  // Только видимость логина/поиска — НЕ трогает опрос Spotify (см. showSpotifyScreen() ниже):
  // раньше опрос /me/player запускался сразу при входе и работал в фоне постоянно, даже пока
  // пользователь сидит на обычном экране управления — это, судя по всему, и мешало старому
  // виджету Now Playing (управляет тем же Arylic напрямую через его локальный HTTP API,
  // handlePlayback()/arylicSendPlayerCommand() в web_control.cpp) — "next" там начал приводить
  // к паузе после того, как добавился параллельный опрос через облачный Spotify Web API
  "function renderSpotifyAuth(){"
  "let logged=!!spTok();"
  "document.getElementById('spLoginWrap').style.display=logged?'none':'block';"
  "document.getElementById('spAppWrap').style.display=logged?'block':'none'}"
  // Полноэкранное переключение между обычным управлением и Spotify — весь #controlWrap
  // прячется целиком (эквалайзер/шапка/меню/экран Power Off, что бы из двух сейчас ни было
  // показано — applyPowerState() сама разберётся при возврате), #spotifyScreen занимает его
  // место. Кнопка входа — в шапке #controlWrap, кнопка "Назад" — в самом #spotifyScreen.
  // Опрос Spotify (spStartPolling()) включается и выключается вместе с этим экраном — не
  // должен работать в фоне, пока пользователь смотрит на обычное управление (см. комментарий
  // у renderSpotifyAuth() выше)
  "function showSpotifyScreen(){"
  "document.getElementById('controlWrap').style.display='none';"
  "document.getElementById('spotifyScreen').style.display='block';"
  "if(spTok()){spEnsureDevice();spStartPolling()}}"
  "function showControlScreen(){"
  "document.getElementById('spotifyScreen').style.display='none';"
  "document.getElementById('controlWrap').style.display='block';"
  "if(spPollTimer){clearInterval(spPollTimer);spPollTimer=null}"
  // Возврат в управление сбрасывает экран артиста/плейлиста, если он был открыт — следующий
  // заход на Spotify начинается заново с поиска/библиотеки, а не оставляет вас внутри чужого
  // артиста, про которого вы уже забыли
  "closeDetailView();"
  "lastPoweredOff=null;applyPowerState(lastStatus?lastStatus.poweredOff:false)}"
  "let spDeviceId=null;try{spDeviceId=localStorage.getItem('spDeviceId')||null}catch(e){}"
  "async function spEnsureDevice(){"
  "try{"
  "let d=await spApi('/me/player/devices');let list=d.devices||[];"
  "if(spDeviceId&&list.some(x=>x.id===spDeviceId)){"
  "spSetDeviceLabel(list.find(x=>x.id===spDeviceId).name);return}"
  "let arylic=list.find(x=>/arylic|soundsystem/i.test(x.name));"
  "if(arylic){spDeviceId=arylic.id;try{localStorage.setItem('spDeviceId',spDeviceId)}catch(e){}"
  "spSetDeviceLabel(arylic.name);return}"
  "spSetDeviceLabel(null)"
  "}catch(e){}}"
  "function spSetDeviceLabel(name){"
  "document.getElementById('spDeviceTag').innerText=name?('\\u25b6 '+name):I18N[currentLang].spDeviceNotFound}"
  // offset — с какой позиции в uris начать: без него Spotify не строит очередь вокруг
  // выбранного трека (см. вызовы ниже) — раньше запускался только САМ этот трек без соседей,
  // из-за чего "next" упирался в пустоту (нечего листать) вместо перехода к следующему в
  // "Моих треках"/результатах поиска — именно так и проявлялся баг "играет одну песню и стоп"
  "async function spPlayUris(uris,offset){"
  "try{await spApi('/me/player/play'+spQS([spDevParam()]),{method:'PUT',"
  "body:JSON.stringify(Object.assign({uris},offset?{offset}:{}))});"
  "setTimeout(spPollPlayback,500)}catch(e){alert(I18N[currentLang].spPlayFailed+e.message)}}"
  // context_uri (плейлист/альбом целиком), не голый список uris — для плейлиста это ближе к
  // тому, что реально делает сам Spotify: очередь строится из настоящего плейлиста, а не из
  // скопированного на момент клика списка id (тот не переживёт правки плейлиста кем-то другим)
  "async function spPlayContext(contextUri,offset){"
  "try{await spApi('/me/player/play'+spQS([spDevParam()]),{method:'PUT',"
  "body:JSON.stringify(Object.assign({context_uri:contextUri},offset?{offset}:{}))});"
  "setTimeout(spPollPlayback,500)}catch(e){alert(I18N[currentLang].spPlayFailed+e.message)}}"
  "async function spResume(){try{await spApi('/me/player/play'+spQS([spDevParam()]),{method:'PUT'});"
  "setTimeout(spPollPlayback,400)}catch(e){alert(I18N[currentLang].spPlayerError+e.message)}}"
  "async function spPause(){try{await spApi('/me/player/pause'+spQS([spDevParam()]),{method:'PUT'});"
  "setTimeout(spPollPlayback,400)}catch(e){alert(I18N[currentLang].spPlayerError+e.message)}}"
  "async function spNext(){try{await spApi('/me/player/next'+spQS([spDevParam()]),{method:'POST'});"
  "setTimeout(spPollPlayback,400)}catch(e){alert(I18N[currentLang].spPlayerError+e.message)}}"
  "async function spPrev(){try{await spApi('/me/player/previous'+spQS([spDevParam()]),{method:'POST'});"
  "setTimeout(spPollPlayback,400)}catch(e){alert(I18N[currentLang].spPlayerError+e.message)}}"
  "function spTogglePlayPause(){if(spLastPlayback&&spLastPlayback.is_playing)spPause();else spResume()}"
  "function spSeekChanged(val){spSeekDragging=false;"
  "if(spLastPlayback&&spLastPlayback.item){"
  "spApi('/me/player/seek'+spQS(['position_ms='+Math.round(val/1000*spLastPlayback.item.duration_ms),spDevParam()]),"
  "{method:'PUT'}).catch(e=>alert(I18N[currentLang].spSeekFailed+e.message))}}"
  "function spVolChanged(val){spVolDragging=false;"
  "spApi('/me/player/volume'+spQS(['volume_percent='+Math.round(val),spDevParam()]),{method:'PUT'}).catch(()=>{})}"
  "const SP_HEART='<svg viewBox=\"0 0 24 24\" width=18 height=18><path d=\"M12 21s-7.5-4.6-10-9.1C0.3 8.6 1.8 5"
  " 5.3 5c2 0 3.4 1 4.7 2.6C11.3 6 12.7 5 14.7 5c3.5 0 5 3.6 3.3 6.9C19.5 16.4 12 21 12 21z\"></path></svg>';"
  "function spImg(images){return images&&images.length?images[images.length-1].url:''}"
  "function spTrackRow(track,onPlay){"
  "let row=document.createElement('div');row.className='spRow';"
  "let img=document.createElement('img');img.className='spThumb';img.src=spImg(track.album&&track.album.images);"
  "row.appendChild(img);"
  "let meta=document.createElement('div');meta.className='spMeta';"
  "let name=document.createElement('div');name.className='spName';name.innerText=track.name;"
  "let sub=document.createElement('div');sub.className='spSub';"
  "sub.innerText=(track.artists||[]).map(a=>a.name).join(', ');"
  "meta.appendChild(name);meta.appendChild(sub);row.appendChild(meta);"
  "let qBtn=document.createElement('button');qBtn.className='spPillBtn';"
  "qBtn.title=I18N[currentLang].spQueueAddTitle;qBtn.innerText=I18N[currentLang].spQueueAddBtn;"
  "qBtn.onclick=function(e){e.stopPropagation();spAddToQueue(track.uri,qBtn)};"
  "row.appendChild(qBtn);"
  "let plBtn=document.createElement('button');plBtn.className='spPillBtn';"
  "plBtn.title=I18N[currentLang].spPlaylistAddTitle;plBtn.innerText=I18N[currentLang].spPlaylistAddBtn;"
  "plBtn.onclick=function(e){e.stopPropagation();openPlaylistPicker(track.uri)};"
  "row.appendChild(plBtn);"
  // Ключ — track.uri (полный Spotify URI, spotify:track:xxx), не голый id: новый унифицированный
  // /me/library работает по URI (см. spToggleSaved()/spMarkSaved() ниже) — эндпоинты по голому
  // id (PUT/DELETE /me/tracks) Spotify деприкейтил в феврале 2026 в пользу этого
  "let heart=document.createElement('button');heart.className='spHeart';heart.innerHTML=SP_HEART;"
  "heart.dataset.uri=track.uri;"
  "heart.onclick=function(e){e.stopPropagation();spToggleSaved(track.uri,heart)};"
  "row.appendChild(heart);"
  "row.onclick=onPlay;"
  "return row}"
  // Универсальная строка для артиста/плейлиста — без сердечка (сохранение артистов/плейлистов
  // не входит в этот экран), img может быть пустой (без обложки/аватара — просто placeholder-фон)
  "function spSimpleRow(name,sub,img,round,onClick){"
  "let row=document.createElement('div');row.className='spRow';"
  "let im=document.createElement('img');im.className='spThumb'+(round?' spThumbRound':'');im.src=img||'';"
  "row.appendChild(im);"
  "let meta=document.createElement('div');meta.className='spMeta';"
  "let n=document.createElement('div');n.className='spName';n.innerText=name;"
  "let s=document.createElement('div');s.className='spSub';s.innerText=sub||'';"
  "meta.appendChild(n);meta.appendChild(s);row.appendChild(meta);"
  "row.onclick=onClick;"
  "return row}"
  "function spSectionTitle(t){let d=document.createElement('div');d.className='spSectionTitle';d.innerText=t;return d}"
  // PUT/DELETE/GET .../me/tracks (save/remove/contains по голому id) Spotify в февральском 2026
  // сносе для Development Mode деприкейтил — но, в отличие от первого впечатления при живом
  // тесте (403 на них), замена реально есть: унифицированный /me/library, работающий по полному
  // Spotify URI (не голому id) сразу для track/album/episode/show/audiobook/user/playlist,
  // до 40 штук за раз через query-параметр uris (см. docs/spotify/API_STATUS.md за подробностями
  // по всему остальному API). Scope тот же, что и был — user-library-modify/user-library-read
  "async function spMarkSaved(uris){"
  "if(!uris.length)return;"
  "try{let flags=await spApi('/me/library/contains?uris='+uris.map(encodeURIComponent).join(','));"
  "uris.forEach((uri,i)=>{if(!flags[i])return;"
  "let h=document.querySelector('.spHeart[data-uri=\"'+uri+'\"]');if(h)h.classList.add('saved')})"
  "}catch(e){}}"
  "async function spToggleSaved(uri,btn){"
  "try{"
  "if(btn.classList.contains('saved')){"
  "await spApi('/me/library?uris='+encodeURIComponent(uri),{method:'DELETE'});btn.classList.remove('saved')}"
  "else{await spApi('/me/library?uris='+encodeURIComponent(uri),{method:'PUT'});btn.classList.add('saved')}"
  "}catch(e){alert(I18N[currentLang].spSaveFailed+e.message)}}"
  "async function spAddToQueue(uri,btn){"
  "try{await spApi('/me/player/queue'+spQS(['uri='+encodeURIComponent(uri),spDevParam()]),{method:'POST'});"
  "let t=btn.innerText;btn.innerText=I18N[currentLang].spQueueAdded;setTimeout(()=>btn.innerText=t,1200)"
  "}catch(e){alert(I18N[currentLang].spQueueFailed+e.message)}}"
  // Выбор плейлиста для "+Плейлист" — свой оверлей (#spPlaylistPicker), не переиспользует
  // #spDetailView, т.к. должен открываться поверх ЛЮБОГО экрана (включая уже открытый
  // #spDetailView артиста/альбома), а не подменять его собой
  "let spPickerTrackUri=null;"
  "async function openPlaylistPicker(uri){"
  "spPickerTrackUri=uri;"
  "document.getElementById('spPlaylistPicker').style.display='block';"
  "let box=document.getElementById('spPlaylistPickerList');"
  "box.innerHTML='<div class=spEmpty>'+I18N[currentLang].spLoading+'</div>';"
  "try{"
  "let d=await spApi('/me/playlists');let items=(d.items||[]).filter(Boolean);"
  "box.innerHTML='';"
  "if(!items.length){box.innerHTML='<div class=spEmpty>'+I18N[currentLang].spNoOwnPlaylists+'</div>';return}"
  "items.forEach(p=>box.appendChild(spSimpleRow(p.name,"
  "I18N[currentLang].spPlaylistSingular+(p.owner&&p.owner.display_name?' · '+p.owner.display_name:''),"
  "spImg(p.images),false,()=>spAddTrackToPlaylist(p.id,p.name))))"
  "}catch(e){box.innerHTML='<div class=spEmpty>'+I18N[currentLang].spError+e.message+'</div>'}}"
  "function closePlaylistPicker(){"
  "document.getElementById('spPlaylistPicker').style.display='none';spPickerTrackUri=null}"
  "async function spAddTrackToPlaylist(playlistId,playlistName){"
  "try{"
  "await spApi('/playlists/'+playlistId+'/items',{method:'POST',body:JSON.stringify({uris:[spPickerTrackUri]})});"
  "closePlaylistPicker();"
  "alert(I18N[currentLang].spAddedToPlaylistPrefix+playlistName+I18N[currentLang].spAddedToPlaylistSuffix)"
  "}catch(e){alert(I18N[currentLang].spAddToPlaylistFailed+e.message)}}"
  // public:false по умолчанию — для домашнего использования приватный плейлист логичнее как
  // умолчание, публичным его всегда можно сделать потом вручную в самом Spotify
  "async function spCreatePlaylistAndAdd(){"
  "let name=prompt(I18N[currentLang].spNewPlaylistPrompt);if(!name)return;"
  "try{"
  "let p=await spApi('/me/playlists',{method:'POST',body:JSON.stringify({name,public:false})});"
  "await spAddTrackToPlaylist(p.id,p.name)"
  "}catch(e){alert(I18N[currentLang].spCreatePlaylistFailed+e.message)}}"
  "async function spToggleShuffle(){"
  "try{"
  "let state=!(spLastPlayback&&spLastPlayback.shuffle_state);"
  "await spApi('/me/player/shuffle'+spQS(['state='+state,spDevParam()]),{method:'PUT'});"
  "setTimeout(spPollPlayback,400)"
  "}catch(e){alert(I18N[currentLang].spPlayerError+e.message)}}"
  "async function spCycleRepeat(){"
  "try{"
  "let cur=(spLastPlayback&&spLastPlayback.repeat_state)||'off';"
  "let next=cur==='off'?'context':cur==='context'?'track':'off';"
  "await spApi('/me/player/repeat'+spQS(['state='+next,spDevParam()]),{method:'PUT'});"
  "setTimeout(spPollPlayback,400)"
  "}catch(e){alert(I18N[currentLang].spPlayerError+e.message)}}"
  // Очередь Spotify не поддерживает переход к произвольному треку внутри себя (только "next"
  // на один вперёд) — строки тут не кликабельны для воспроизведения (onPlay — пустая функция),
  // это просто обзор "что сейчас/что дальше", не ещё один плеер
  "async function openQueueView(){"
  "showDetailView();"
  "let header=document.getElementById('spDetailHeader');header.innerHTML='';"
  "let title=document.createElement('div');title.style.cssText='font-size:1.2em;color:#fff';"
  "title.innerText=I18N[currentLang].spQueueBtn;header.appendChild(title);"
  "let list=document.getElementById('spDetailList');"
  "list.innerHTML='<div class=spEmpty>'+I18N[currentLang].spLoading+'</div>';"
  "try{"
  "let d=await spApi('/me/player/queue');"
  "list.innerHTML='';"
  "if(d.currently_playing){"
  "list.appendChild(spSectionTitle(I18N[currentLang].spNowPlaying));"
  "list.appendChild(spTrackRow(d.currently_playing,()=>{}))}"
  "let queue=d.queue||[];"
  "if(queue.length){"
  "list.appendChild(spSectionTitle(I18N[currentLang].spUpNext));"
  "queue.forEach(t=>list.appendChild(spTrackRow(t,()=>{})))}"
  "if(!d.currently_playing&&!queue.length)"
  "list.innerHTML='<div class=spEmpty>'+I18N[currentLang].spQueueEmpty+'</div>'"
  "}catch(e){list.innerHTML='<div class=spEmpty>'+I18N[currentLang].spError+e.message+'</div>'}}"
  "function spSwitchTab(tab){"
  "document.getElementById('spSearchTab').style.display=tab==='search'?'block':'none';"
  "document.getElementById('spLibTab').style.display=tab==='lib'?'block':'none';"
  "document.getElementById('spAlbumsTab').style.display=tab==='albums'?'block':'none';"
  "document.getElementById('spPlaylistsTab').style.display=tab==='playlists'?'block':'none';"
  "document.getElementById('spRecentTab').style.display=tab==='recent'?'block':'none';"
  "document.getElementById('spTopTab').style.display=tab==='top'?'block':'none';"
  "document.getElementById('spTabSearchBtn').classList.toggle('active',tab==='search');"
  "document.getElementById('spTabLibBtn').classList.toggle('active',tab==='lib');"
  "document.getElementById('spTabAlbumsBtn').classList.toggle('active',tab==='albums');"
  "document.getElementById('spTabPlaylistsBtn').classList.toggle('active',tab==='playlists');"
  "document.getElementById('spTabRecentBtn').classList.toggle('active',tab==='recent');"
  "document.getElementById('spTabTopBtn').classList.toggle('active',tab==='top');"
  "if(tab==='lib'&&!document.getElementById('spLibResults').childElementCount)spLoadLibrary(true);"
  "if(tab==='albums'&&!document.getElementById('spAlbumsResults').childElementCount)spLoadAlbums(true);"
  "if(tab==='playlists'&&!document.getElementById('spPlaylistsResults').childElementCount)spLoadPlaylists(true);"
  "if(tab==='recent'&&!document.getElementById('spRecentResults').childElementCount)spLoadRecent(true);"
  "if(tab==='top'&&!document.getElementById('spTopResults').childElementCount)spLoadTop()}"
  // Только очистка — не живой поиск: пробовали debounce на каждый ввод, не понравилось.
  // Сам поиск всё ещё только по кнопке "Найти", это лишь убирает зависшие с прошлого
  // запроса результаты, когда поле опустело
  "function spClearSearchIfEmpty(){"
  "if(!document.getElementById('spSearchInput').value.trim())"
  "document.getElementById('spSearchResults').innerHTML=''}"
  "async function spSearch(){"
  "let q=document.getElementById('spSearchInput').value.trim();"
  "let box=document.getElementById('spSearchResults');"
  "if(!q){box.innerHTML='';return}"
  "box.innerHTML='<div class=spEmpty>'+I18N[currentLang].spSearching+'</div>';"
  "try{"
  // URLSearchParams вместо ручной склейки строк — сама кодирует спецсимволы/кириллицу
  // корректно. limit=15 когда-то давал "invalid limit" без видимой причины — оказалось,
  // не случайность: в февральском сносе 2026 Spotify тихо урезал /search limit (максимум
  // 50->10, умолчание 20->5) вместе с кучей других эндпоинтов (см. openArtistDetail() ниже
  // за подробным списком). Явно просим потолок (10) — иначе получили бы всего 5 на каждый тип.
  // Три типа сразу одним запросом — Spotify поддерживает несколько type через запятую
  "let d=await spApi('/search?'+new URLSearchParams({q,type:'track,artist,playlist',limit:'10'}));"
  "let tracks=(d.tracks&&d.tracks.items)||[];"
  "let artists=(d.artists&&d.artists.items)||[];"
  // playlists.items может содержать null (удалённый/приватный плейлист в выдаче) — Spotify
  // сам так отдаёт, не наш баг, просто отфильтровываем
  "let playlists=(d.playlists&&d.playlists.items.filter(Boolean))||[];"
  "box.innerHTML='';"
  "if(!tracks.length&&!artists.length&&!playlists.length){"
  "box.innerHTML='<div class=spEmpty>'+I18N[currentLang].spNothingFound+'</div>';return}"
  "if(tracks.length){"
  "box.appendChild(spSectionTitle(I18N[currentLang].spTracksLabel));"
  "let list=document.createElement('div');list.className='spRowList';"
  "let uris=tracks.map(t=>t.uri);"
  "tracks.forEach((t,i)=>list.appendChild(spTrackRow(t,()=>spPlayUris(uris,{position:i}))));"
  "box.appendChild(list);spMarkSaved(uris)}"
  "if(artists.length){"
  "box.appendChild(spSectionTitle(I18N[currentLang].spArtistsLabel));"
  "let list=document.createElement('div');list.className='spRowList';"
  "artists.forEach(a=>list.appendChild(spSimpleRow(a.name,I18N[currentLang].spArtistSingular,"
  "spImg(a.images),true,()=>openArtistDetail(a))));"
  "box.appendChild(list)}"
  "if(playlists.length){"
  "box.appendChild(spSectionTitle(I18N[currentLang].spPlaylistsLabelPlural));"
  "let list=document.createElement('div');list.className='spRowList';"
  "playlists.forEach(p=>list.appendChild(spSimpleRow(p.name,"
  "I18N[currentLang].spPlaylistSingular+(p.owner&&p.owner.display_name?' · '+p.owner.display_name:''),"
  "spImg(p.images),false,()=>openPlaylistDetail(p))));"
  "box.appendChild(list)}"
  "}catch(e){box.innerHTML='<div class=spEmpty>'+I18N[currentLang].spSearchError+e.message+'</div>'}}"
  // Экран артиста/плейлиста — подменяет #spAppWrap целиком, результаты поиска/библиотека при
  // этом скрываются (не остаются позади/под ним). closeDetailView() возвращает как было
  "function showDetailView(){"
  "document.getElementById('spAppWrap').style.display='none';"
  "document.getElementById('spDetailView').style.display='block'}"
  "function closeDetailView(){"
  "document.getElementById('spDetailView').style.display='none';"
  "document.getElementById('spAppWrap').style.display='block'}"
  "async function openArtistDetail(artist){"
  "showDetailView();"
  "let header=document.getElementById('spDetailHeader');header.innerHTML='';"
  "let img=spImg(artist.images);"
  "if(img){let im=document.createElement('img');im.src=img;"
  "im.style.cssText='width:120px;height:120px;border-radius:50%;object-fit:cover';header.appendChild(im)}"
  "let name=document.createElement('div');name.style.cssText='font-size:1.2em;color:#fff;margin-top:8px';"
  "name.innerText=artist.name;header.appendChild(name);"
  "let list=document.getElementById('spDetailList');"
  "list.innerHTML='<div class=spEmpty>'+I18N[currentLang].spLoading+'</div>';"
  "try{"
  // GET /artists/{id}/top-tracks полностью убран Spotify для Development Mode приложений
  // в феврале 2026 (вместе с popularity/followers у артиста и /browse/new-releases) — никакой
  // market/страна тут уже не спасают, эндпоинт отвечает 403 всем, у кого нет Extended Quota
  // Mode (порог входа — 250K MAU, недостижим для домашнего проекта). Поиск /search?q=artist:"…"
  // тоже не годится заменой — фразовый фильтр находит только то, что попало в топ релевантности
  // текстового индекса, на практике иногда буквально 2-3 трека вместо всего каталога артиста.
  // Настоящая замена — собственная дискография: /artists/{id}/albums жив (не в списке снесённых).
  // Раньше треки каждого альбома брались одним батч-вызовом /albums?ids=… — но GET /albums
  // (batch "Get Several Albums") Spotify снёс в том же февральском сносе 2026, что и top-tracks
  // (список снесённого широкий: batch-эндпоинты /albums, /artists, /tracks и др., save/unsave
  // треков, GET /playlists/{id}/tracks и многое другое — не только top-tracks). Единственное,
  // что осталось — брать альбомы по одному через GET /albums/{id} (не batch, её не тронули);
  // берём их параллельно, а не последовательно, чтобы не ждать round-trip'ы подряд. limit не
  // задаём явно на /artists/{id}/albums — как и у /search, явный limit тут тоже может словить
  // "invalid limit"; но раз тот же февральский снос тихо срезал умолчательный/максимальный limit
  // и здесь (было 50, стало 10 — не упомянуто ни в одном changelog, видно только по факту, см.
  // docs/spotify/API_STATUS.md), одной страницы теперь мало даже для не самых плодовитых
  // артистов — догружаем страницы через offset, пока не наберём достаточно или список не кончится
  "let albIds=[],albOffset=0;"
  "for(let page=0;page<4;page++){"
  "let albList=await spApi('/artists/'+artist.id+'/albums?'+"
  "new URLSearchParams({include_groups:'album,single',offset:albOffset}));"
  "let items=albList.items||[];"
  "albIds=albIds.concat(items.map(a=>a.id));"
  "if(!albList.next||!items.length)break;"
  "albOffset+=items.length}"
  "let albs=await Promise.all(albIds.map(id=>spApi('/albums/'+id).catch(()=>null)));"
  "let tracks=[];"
  "albs.forEach(alb=>{if(!alb)return;"
  "(alb.tracks&&alb.tracks.items||[]).forEach(t=>{t.album={images:alb.images};tracks.push(t)})});"
  // Один и тот же трек часто встречается и в альбоме, и синглом, и в переиздании — они получают
  // разные id, поэтому дедуп по имени+исполнителям, не по id, оставляя первое вхождение
  "let seen=new Set();tracks=tracks.filter(t=>{"
  "let key=(t.name+'|'+(t.artists||[]).map(a=>a.id).join(',')).toLowerCase();"
  "if(seen.has(key))return false;seen.add(key);return true});"
  "list.innerHTML='';"
  "if(!tracks.length){list.innerHTML='<div class=spEmpty>'+I18N[currentLang].spNoData+'</div>';return}"
  "let uris=tracks.map(t=>t.uri);"
  "tracks.forEach((t,i)=>list.appendChild(spTrackRow(t,()=>spPlayUris(uris,{position:i}))));"
  "spMarkSaved(uris)"
  "}catch(e){list.innerHTML='<div class=spEmpty>'+I18N[currentLang].spError+e.message+'</div>'}}"
  "async function openPlaylistDetail(playlist){"
  "showDetailView();"
  "let header=document.getElementById('spDetailHeader');header.innerHTML='';"
  "let img=spImg(playlist.images);"
  "if(img){let im=document.createElement('img');im.src=img;"
  "im.style.cssText='width:120px;height:120px;border-radius:8px;object-fit:cover';header.appendChild(im)}"
  "let name=document.createElement('div');name.style.cssText='font-size:1.2em;color:#fff;margin-top:8px';"
  "name.innerText=playlist.name;header.appendChild(name);"
  "let sub=document.createElement('div');sub.style.cssText='font-size:.8em;color:#999';"
  "sub.innerText=(playlist.owner&&playlist.owner.display_name)||'';header.appendChild(sub);"
  "let list=document.getElementById('spDetailList');"
  "list.innerHTML='<div class=spEmpty>'+I18N[currentLang].spLoading+'</div>';"
  "try{"
  // Старый GET /playlists/{id}/tracks снесён Spotify в февральском 2026 сносе для Development
  // Mode вместе с top-tracks/batch-эндпоинтами (см. openArtistDetail() выше) — но взамен
  // появился отдельный GET /playlists/{id}/items (не просто переименование: он и правда живой,
  // не в списке снесённого). У него ещё и поле трека внутри каждой записи переименовано —
  // .track стал .item (старое имя формально осталось как deprecated-алиас, но полагаться на
  // него не стоит). По документации Spotify этот эндпоинт отдаёт содержимое только для
  // плейлистов, которыми владеет текущий пользователь, или в которых он соавтор — для чужого
  // плейлиста (найденного через поиск) он отвечает не пустым списком, а прямо 403 Forbidden
  // (живой тест) — ловим это отдельно в catch, а не показываем голый "Ошибка: Forbidden"
  "let d=await spApi('/playlists/'+playlist.id+'/items');"
  "let tracks=(d.items||[]).map(it=>it.item||it.track).filter(Boolean);"
  "list.innerHTML='';"
  "if(!tracks.length){list.innerHTML='<div class=spEmpty>'+I18N[currentLang].spEmpty+'</div>';return}"
  // context_uri (сам плейлист), не uris — так Spotify ведёт очередь как "играю этот плейлист"
  // по-настоящему (в отличие от простого списка uris), офсет — с какого трека начать
  "tracks.forEach((t,i)=>list.appendChild(spTrackRow(t,()=>spPlayContext(playlist.uri,{position:i}))));"
  "spMarkSaved(tracks.map(t=>t.uri).filter(Boolean))"
  "}catch(e){list.innerHTML=e.status===403?"
  "'<div class=spEmpty>'+I18N[currentLang].spNotYourPlaylist+'</div>':"
  "'<div class=spEmpty>'+I18N[currentLang].spError+e.message+'</div>'}}"
  "let spLibOffset=0,spLibUris=[];"
  "async function spLoadLibrary(reset){"
  "if(reset){spLibOffset=0;spLibUris=[];document.getElementById('spLibResults').innerHTML=''}"
  "let box=document.getElementById('spLibResults');"
  "try{"
  "let d=await spApi('/me/tracks?limit=30&offset='+spLibOffset);"
  "let tracks=d.items.map(it=>it.track);"
  "if(!tracks.length&&spLibOffset===0){"
  "box.innerHTML='<div class=spEmpty>'+I18N[currentLang].spNoSavedTracks+'</div>';return}"
  // Копим URI всех уже подгруженных страниц в spLibUris — очередь строится по НЕМУ (позиция —
  // сквозная по всему списку), не только по текущей странице, иначе "next" на последнем треке
  // текущей порции просто упирался бы в конец, даже если дальше есть ещё подгруженные "Показать ещё"
  "let startIndex=spLibUris.length;"
  "spLibUris=spLibUris.concat(tracks.map(t=>t.uri));"
  "tracks.forEach((t,i)=>{"
  "let row=spTrackRow(t,()=>spPlayUris(spLibUris,{position:startIndex+i}));box.appendChild(row);"
  "let h=row.querySelector('.spHeart');if(h)h.classList.add('saved')"
  "});"
  "spLibOffset+=tracks.length;"
  "document.getElementById('spLibMoreBtn').style.display=d.next?'block':'none'"
  "}catch(e){box.innerHTML='<div class=spEmpty>'+I18N[currentLang].spError+e.message+'</div>'}}"
  "let spPlOffset=0;"
  "async function spLoadPlaylists(reset){"
  "if(reset){spPlOffset=0;document.getElementById('spPlaylistsResults').innerHTML=''}"
  "let box=document.getElementById('spPlaylistsResults');"
  "try{"
  // limit не задаём — тот же принцип, что у /search/albums выше: без явного значения меньше
  // риска словить "invalid limit", а /me/playlists и так постранично догружается по offset
  "let d=await spApi('/me/playlists?offset='+spPlOffset);"
  "let items=(d.items||[]).filter(Boolean);"
  "if(!items.length&&spPlOffset===0){"
  "box.innerHTML='<div class=spEmpty>'+I18N[currentLang].spNoPlaylists+'</div>';return}"
  "items.forEach(p=>box.appendChild(spSimpleRow(p.name,"
  "I18N[currentLang].spPlaylistSingular+(p.owner&&p.owner.display_name?' · '+p.owner.display_name:''),"
  "spImg(p.images),false,()=>openPlaylistDetail(p))));"
  "spPlOffset+=items.length;"
  "document.getElementById('spPlaylistsMoreBtn').style.display=d.next?'block':'none'"
  "}catch(e){box.innerHTML='<div class=spEmpty>'+I18N[currentLang].spError+e.message+'</div>'}}"
  "async function openAlbumDetail(album){"
  "showDetailView();"
  "let header=document.getElementById('spDetailHeader');header.innerHTML='';"
  "let img=spImg(album.images);"
  "if(img){let im=document.createElement('img');im.src=img;"
  "im.style.cssText='width:120px;height:120px;border-radius:8px;object-fit:cover';header.appendChild(im)}"
  "let name=document.createElement('div');name.style.cssText='font-size:1.2em;color:#fff;margin-top:8px';"
  "name.innerText=album.name;header.appendChild(name);"
  "let sub=document.createElement('div');sub.style.cssText='font-size:.8em;color:#999';"
  "sub.innerText=(album.artists||[]).map(a=>a.name).join(', ');header.appendChild(sub);"
  "let list=document.getElementById('spDetailList');"
  "list.innerHTML='<div class=spEmpty>'+I18N[currentLang].spLoading+'</div>';"
  "try{"
  "let full=await spApi('/albums/'+album.id);"
  "let tracks=(full.tracks&&full.tracks.items||[]).map(t=>{t.album={images:full.images};return t});"
  "list.innerHTML='';"
  "if(!tracks.length){list.innerHTML='<div class=spEmpty>'+I18N[currentLang].spNoData+'</div>';return}"
  "let uris=tracks.map(t=>t.uri);"
  "tracks.forEach((t,i)=>list.appendChild(spTrackRow(t,()=>spPlayContext(album.uri,{position:i}))));"
  "spMarkSaved(uris)"
  "}catch(e){list.innerHTML='<div class=spEmpty>'+I18N[currentLang].spError+e.message+'</div>'}}"
  "let spAlbOffset=0;"
  "async function spLoadAlbums(reset){"
  "if(reset){spAlbOffset=0;document.getElementById('spAlbumsResults').innerHTML=''}"
  "let box=document.getElementById('spAlbumsResults');"
  "try{"
  "let d=await spApi('/me/albums?offset='+spAlbOffset);"
  "let items=(d.items||[]).filter(Boolean);"
  "if(!items.length&&spAlbOffset===0){"
  "box.innerHTML='<div class=spEmpty>'+I18N[currentLang].spNoSavedAlbums+'</div>';return}"
  "items.forEach(it=>box.appendChild(spSimpleRow(it.album.name,"
  "(it.album.artists||[]).map(a=>a.name).join(', '),"
  "spImg(it.album.images),false,()=>openAlbumDetail(it.album))));"
  "spAlbOffset+=items.length;"
  "document.getElementById('spAlbumsMoreBtn').style.display=d.next?'block':'none'"
  "}catch(e){box.innerHTML='<div class=spEmpty>'+I18N[currentLang].spError+e.message+'</div>'}}"
  // Недавно проигранное — курсорная пагинация (before), не offset, как у большинства остальных
  // списков: Spotify отдаёт cursors.before для следующей (более старой) страницы
  "let spRecentBefore=null;"
  "async function spLoadRecent(reset){"
  "if(reset){spRecentBefore=null;document.getElementById('spRecentResults').innerHTML=''}"
  "let box=document.getElementById('spRecentResults');"
  "try{"
  "let d=await spApi('/me/player/recently-played'+spQS([spRecentBefore?('before='+spRecentBefore):'']));"
  "let items=d.items||[];"
  "if(!items.length&&!spRecentBefore){"
  "box.innerHTML='<div class=spEmpty>'+I18N[currentLang].spRecentEmpty+'</div>';return}"
  "items.forEach(it=>{if(!it.track)return;"
  "box.appendChild(spTrackRow(it.track,()=>spPlayUris([it.track.uri],{position:0})))});"
  "spMarkSaved(items.map(it=>it.track&&it.track.uri).filter(Boolean));"
  "spRecentBefore=d.cursors&&d.cursors.before;"
  "document.getElementById('spRecentMoreBtn').style.display=(d.next&&spRecentBefore)?'block':'none'"
  "}catch(e){box.innerHTML='<div class=spEmpty>'+I18N[currentLang].spError+e.message+'</div>'}}"
  // "Топ" — личный топ (не путать со снесённым top-tracks АРТИСТА, см. openArtistDetail() выше —
  // это другой, живой эндпоинт: /me/top/*, топ самого пользователя). Подписки на артистов живут
  // в этом же контейнере третьей вкладкой — заводить ради них ещё один пункт в spTabs, которых
  // и так уже много, не стоило
  "let spTopType='tracks',spTopRange='medium_term';"
  "function spSwitchTopType(type){spTopType=type;spLoadTop()}"
  "function spTopRangeChanged(v){spTopRange=v;spLoadTop()}"
  "async function spLoadTop(){"
  "let box=document.getElementById('spTopResults');"
  "document.getElementById('spTopTracksBtn').classList.toggle('active',spTopType==='tracks');"
  "document.getElementById('spTopArtistsBtn').classList.toggle('active',spTopType==='artists');"
  "document.getElementById('spTopFollowingBtn').classList.toggle('active',spTopType==='following');"
  "document.getElementById('spTopRangeSelect').style.display=spTopType==='following'?'none':'inline-block';"
  "box.innerHTML='<div class=spEmpty>'+I18N[currentLang].spLoading+'</div>';"
  "try{"
  "if(spTopType==='following'){"
  // /me/following — единственный эндпоинт тут с другой формой ответа: список не прямо в
  // items верхнего уровня, а вложен под artists (задел на будущее — вдруг Spotify добавит
  // другие типы подписок, кроме артистов)
  "let d=await spApi('/me/following?type=artist&limit=20');"
  "let items=(d.artists&&d.artists.items)||[];"
  "box.innerHTML='';"
  "if(!items.length){box.innerHTML='<div class=spEmpty>'+I18N[currentLang].spNoFollowing+'</div>';return}"
  "items.forEach(a=>box.appendChild(spSimpleRow(a.name,I18N[currentLang].spArtistSingular,"
  "spImg(a.images),true,()=>openArtistDetail(a))));"
  "return}"
  "let d=await spApi('/me/top/'+spTopType+'?'+new URLSearchParams({time_range:spTopRange,limit:'20'}));"
  "let items=d.items||[];"
  "box.innerHTML='';"
  "if(!items.length){box.innerHTML='<div class=spEmpty>'+I18N[currentLang].spNotEnoughData+'</div>';return}"
  "if(spTopType==='tracks'){"
  "let uris=items.map(t=>t.uri);"
  "items.forEach((t,i)=>box.appendChild(spTrackRow(t,()=>spPlayUris(uris,{position:i}))));"
  "spMarkSaved(uris)"
  "}else{"
  "items.forEach(a=>box.appendChild(spSimpleRow(a.name,I18N[currentLang].spArtistSingular,"
  "spImg(a.images),true,()=>openArtistDetail(a))))}"
  "}catch(e){box.innerHTML='<div class=spEmpty>'+I18N[currentLang].spError+e.message+'</div>'}}"
  "let spPollTimer=null,spLastPlayback=null,spSeekDragging=false,spVolDragging=false;"
  "function spStartPolling(){spPollPlayback();if(spPollTimer)clearInterval(spPollTimer);"
  "spPollTimer=setInterval(spPollPlayback,4000)}"
  "async function spPollPlayback(){"
  "try{let pb=await spApi('/me/player');spLastPlayback=pb;spRenderPlayback(pb)}catch(e){}}"
  "function spFmtTime(ms){if(!isFinite(ms)||ms<0)ms=0;let s=Math.floor(ms/1000);"
  "return Math.floor(s/60)+':'+(s%60<10?'0':'')+(s%60)}"
  "function spRenderPlayback(pb){"
  "let bar=document.getElementById('spPlayerBar');"
  "if(!pb||!pb.item){bar.style.display='none';return}"
  "bar.style.display='block';"
  "let item=pb.item;"
  "document.getElementById('spNpThumb').src=spImg(item.album&&item.album.images);"
  "document.getElementById('spNpName').innerText=item.name;"
  "document.getElementById('spNpArtist').innerText=(item.artists||[]).map(a=>a.name).join(', ');"
  "document.getElementById('spPlayPauseIcon').innerHTML=pb.is_playing"
  "?'<svg viewBox=\"0 0 24 24\" width=20 height=20><rect x=5 y=4 width=5 height=16></rect>"
  "<rect x=14 y=4 width=5 height=16></rect></svg>'"
  ":'<svg viewBox=\"0 0 24 24\" width=20 height=20><path d=\"M5 3L20 12L5 21Z\"></path></svg>';"
  "if(!spSeekDragging){"
  "document.getElementById('spSeekSlider').value="
  "item.duration_ms?Math.round(pb.progress_ms/item.duration_ms*1000):0}"
  "document.getElementById('spCur').innerText=spFmtTime(pb.progress_ms);"
  "document.getElementById('spDur').innerText=spFmtTime(item.duration_ms);"
  "if(!spVolDragging&&pb.device)document.getElementById('spVolSlider').value=pb.device.volume_percent;"
  "if(pb.device)spSetDeviceLabel(pb.device.name);"
  "document.getElementById('spShuffleBtn').classList.toggle('active',!!pb.shuffle_state);"
  "let rs=pb.repeat_state||'off';"
  "document.getElementById('spRepeatBtn').classList.toggle('active',rs!=='off');"
  "document.getElementById('spRepeatBtn').innerHTML=rs==='track'?'&#128258;':'&#128257;'}"
  // Возврат из callback.html (см. docs/spotify/callback.html) — токен во фрагменте адреса, не
  // в query, поэтому сервер ESP32 его никогда не видит. Специально в самом конце скрипта, а не
  // сразу после spLogin()/spLogout() выше — раньше был там, и живой тест показал странную
  // поломку (следующий клик по поиску падал на "Cannot access 'SP_HEART' before initialization",
  // хотя тот объявлен ниже по тексту и должен был успеть выполниться до первого клика). Здесь,
  // в самом хвосте, все функции/константы выше уже гарантированно объявлены к моменту вызова
  "(function spCaptureCallback(){"
  "if(location.hash.indexOf('access_token=')===-1)return;"
  "let p=new URLSearchParams(location.hash.slice(1));"
  "spSaveTok({access:p.get('access_token'),refresh:p.get('refresh_token'),"
  "exp:Date.now()+Number(p.get('expires_in'))*1000});"
  "history.replaceState(null,'',location.pathname+location.search);"
  "renderSpotifyAuth();"
  // Раз попали сюда — значит вход начался с #spotifyScreen (spLogin() — единственное место,
  // откуда вообще уходят на login.html), возвращаем ровно туда же, а не оставляем на обычном
  // экране управления, где придётся снова тыкать "Spotify" вручную
  "showSpotifyScreen()"
  "})();"
  "renderSpotifyAuth();"
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
        if (action == "power") {
          // Оптимистично, сразу по клику — настоящее подтверждение от Mega (POWER: по UART,
          // см. applyWebPowerState()) придёт чуть позже и совпадёт с этим же значением
          // (applyWebPowerState() не делает ничего повторно, если оно совпадает)
          applyWebPowerState(!webPoweredOff);
        }
        break;
      }
    }
  }
  server.send(204);
}

// JSON, не готовая строка на русском — текст ("Wi-Fi OK"/"отключён"/"последняя команда")
// теперь собирает и переводит сам клиент (см. renderStatus() в PAGE_HTML, I18N), у ESP32
// своего языка нет и быть не должно
static void handleStatus() {
  String status = "{\"wifi\":";
  status += wifiIsConnected() ? "true" : "false";
  status += ",\"lastCmd\":\"";
  if (lastActionSent != '\0') {
    status += lastActionSent;
  }
  status += "\",\"poweredOff\":";
  status += webPoweredOff ? "true" : "false";
  // Реальное состояние/датчики Mega по UART (см. mega_link.h) — megaKnown==false, пока Mega
  // ни разу не прислала POWER: (например сразу после перезагрузки самого ESP32); значения
  // temps/voltage в этом случае бессмысленны, клиент (renderStatus() в PAGE_HTML) их не рисует
  status += ",\"megaKnown\":";
  status += megaLinkPowerKnown() ? "true" : "false";
  status += ",\"voltageKnown\":";
  status += megaLinkVoltageKnown() ? "true" : "false";
  status += ",\"voltage\":";
  status += megaLinkVoltage();
  status += ",\"temps\":[";
  status += String(megaLinkTemp(0), 1);
  status += ",";
  status += String(megaLinkTemp(1), 1);
  status += ",";
  status += String(megaLinkTemp(2), 1);
  status += "]}";
  server.send(200, "application/json", status);
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
