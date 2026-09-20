# Spotify Web API — статус эндпоинтов для Development Mode (по состоянию на 2026-09-20)

## Зачем этот файл

Справочник для отладки: какие эндпоинты Spotify Web API реально работают для нашего приложения, какие сломаны/урезаны, какие переименовали поля молча (без ошибки — просто пропадает или переименовывается ключ в JSON). Собран после серии багов, вызванных изменениями Spotify Web API с февраля 2026 (см. `2026-02-06-update-on-developer-access-and-platform-security`).

**Применимо к:** наше приложение — Development Mode, `client_id=1a0298a844e24d6d8ab31c9342760d42`, PKCE (без client secret), Extended Quota Mode нам недоступен (требует ≥250k MAU + организацию — см. "Development Mode vs Extended Quota Mode" ниже), т.е. все ограничения ниже актуальны для нас **навсегда**, не временно.

**Источники:**
- Официальный живой OpenAPI-контракт: `https://developer.spotify.com/reference/web-api/open-api-schema.yaml` (скачан и разобран целиком при подготовке этого файла — самый надёжный источник, т.к. это то же самое описание API, из которого генерируется сайт документации)
- Блог: https://developer.spotify.com/blog/2026-02-06-update-on-developer-access-and-platform-security
- Migration guide: https://developer.spotify.com/documentation/web-api/tutorials/february-2026-migration-guide
- Changelog по месяцам: `.../references/changes/{february,march,may,july}-2026` (страниц за апрель/июнь/август/сентябрь 2026 не существует — 404, т.е. в эти месяцы изменений в Web API не публиковали)
- `https://developer.spotify.com/documentation/web-api/concepts/quota-modes`
- Живое тестирование нашим приложением (см. пометки "подтверждено live" ниже)

**Важная методологическая оговорка:** OpenAPI-схема помечает часть эндпоинтов флагом `deprecated: true`, но это **не то же самое**, что "удалён для Development Mode". Флаг `deprecated` в схеме общий для всех уровней доступа (в т.ч. Extended Quota Mode) и означает "используйте новый эндпоинт", но сам по себе не гарантирует, что старый уже возвращает ошибку. Отдельно от этого, changelog за февраль 2026 явно называет часть этих же эндпоинтов "removed" — судя по нашему живому тестированию, для Development Mode это уже жёсткое удаление (403/404), а не просто пометка "не рекомендуется". Ниже это различие показано явно: колонка "Статус" отражает **реальное поведение для Dev Mode** (по live-тесту, где он есть, иначе по формулировке changelog), а не только флаг `deprecated` из схемы.

## Development Mode vs Extended Quota Mode

- Development Mode: до 5 пользователей по allowlist, владелец приложения обязан иметь активную Spotify Premium подписку, один пул квоты на разработчика (не на Client ID — см. июль 2026 ниже), при превышении — `429` с `"reason": "QUOTA_EXCEEDED"` в теле.
- Extended Quota Mode: без ограничения по пользователям и allowlist, более высокий рейт-лимит; заявку принимают только от организаций (не от физлиц), с рабочим корпоративным email, требуется уже запущенный продукт, **≥250 000 MAU**, присутствие на ключевых рынках Spotify, подтверждённая коммерческая жизнеспособность. Рассмотрение заявки — до 6 недель. Источник: https://developer.spotify.com/documentation/web-api/concepts/quota-modes
- Для нашего hobby-проекта Extended Quota Mode недостижим по определению (нет 250k MAU и организации) — все ограничения Dev Mode ниже стоит считать постоянными архитектурными рамками, а не временным неудобством.

## Хронология изменений (все найденные monthly changelog за 2026)

| Месяц | Есть ли changelog-страница | Суть |
|---|---|---|
| Февраль 2026 | Да | Основной пакет: массовые REMOVED/ADDED/CHANGED (см. таблицы ниже) |
| Март 2026 | Да | Откат: `external_ids` для Album и Track **возвращён** — в феврале был анонсирован как убираемое поле, но так и не был убран, откат зафиксирован задним числом |
| Апрель 2026 | Нет (404) | Изменений не было |
| Май 2026 | Да | Добавлено поле `account_id` в объект пользователя (см. "Users/Профиль" ниже) |
| Июнь 2026 | Нет (404) | Изменений не было |
| Июль 2026 | Да | Лимит Client ID на разработчика поднят с 1 до 25; квота считается на аккаунт разработчика, а не на Client ID; структура ошибки 429 дополнена `"reason": "QUOTA_EXCEEDED"` |
| Август 2026 | Нет (404) | Изменений не было |
| Сентябрь 2026 | Нет (404) | Изменений не было (на сегодня, 2026-09-20) |

Изначальный анонс (блог, 6 февраля) описывал более жёсткий график: новые правила — с 11 февраля для новых Client ID, обязательное соответствие для существующих — с 9 марта. По формулировке блога часть деталей эндпоинт-доступа для уже существующих интеграций **откладывалась** после фидбека сообщества, но фактический changelog за февраль и разобранная нами OpenAPI-схема показывают, что основной пакет удалений/переименований в итоге всё же применён и действует сейчас (2026-09-20) — если наблюдаете отличающееся поведение от описанного здесь, перепроверьте эту страницу блога напрямую, могли быть дальнейшие точечные послабления, которые не попали ни в один monthly changelog.

## Таблица эндпоинтов по категориям

Легенда: ✅ Works · ❌ Removed (для Dev Mode) · ⚠️ Restricted · ❓ Unclear/не проверено нами живьём

### Playback control (Player) — вообще не затронуто

| Эндпоинт | Статус | Scope | Заметки |
|---|---|---|---|
| `GET /me/player` | ✅ | `user-read-playback-state` | Без изменений |
| `PUT /me/player` (transfer) | ✅ | `user-modify-playback-state` | Без изменений |
| `GET /me/player/devices` | ✅ | `user-read-playback-state` | Без изменений |
| `GET /me/player/currently-playing` | ✅ | `user-read-currently-playing` | Без изменений |
| `PUT /me/player/play` | ✅ | `user-modify-playback-state` | Без изменений |
| `PUT /me/player/pause` | ✅ | `user-modify-playback-state` | Без изменений |
| `POST /me/player/next` / `/previous` | ✅ | `user-modify-playback-state` | Без изменений |
| `PUT /me/player/seek` | ✅ | `user-modify-playback-state` | Без изменений |
| `PUT /me/player/repeat` / `/shuffle` / `/volume` | ✅ | `user-modify-playback-state` | Без изменений |
| `GET /me/player/recently-played` | ✅ | `user-read-recently-played` | Без изменений, `limit` до 50 (общий параметр, не урезан) |
| `GET`/`POST /me/player/queue` | ✅ | `user-read-currently-playing`+`user-read-playback-state` / `user-modify-playback-state` | Без изменений |

Все Player-эндпоинты в OpenAPI-схеме **не помечены** `deprecated`. Подтверждено нашим живым использованием — работает без нареканий уже сейчас в проде.

### Search

| Эндпоинт | Статус | Scope | Заметки |
|---|---|---|---|
| `GET /search` | ⚠️ | нет (публичный) | **Подтверждено live**: `limit` максимум урезан с 50 до **10**, дефолт — с 20 до **5**. Запрос с `limit>10` теперь ошибка (раньше — тихо клампился/работал). См. "Известные ловушки" ниже |

### Albums / Artists / Tracks / Shows / Episodes / Audiobooks / Chapters — единичные живы, батчи мертвы

| Эндпоинт | Статус | Scope | Заметки |
|---|---|---|---|
| `GET /albums/{id}` | ✅ | нет | Жив |
| `GET /albums` (batch) | ❌ | нет | **Подтверждено live (403)**. `deprecated: true` в схеме. Замены batch-эндпоинта нет — только по одному ID |
| `GET /albums/{id}/tracks` | ✅ | нет | Жив, не деприкейтед |
| `GET /artists/{id}` | ✅ | нет | Жив |
| `GET /artists` (batch) | ❌ | нет | **Подтверждено live (403)**. `deprecated: true` |
| `GET /artists/{id}/albums` | ✅ | нет | Жив; `limit` теперь максимум **10** (не 50, как раньше) — не было в контексте задачи, свежая находка |
| `GET /artists/{id}/top-tracks` | ❌ | нет | **Подтверждено live (403)**. `deprecated: true`. Без замены — популярных треков артиста через API теперь не получить вообще |
| `GET /artists/{id}/related-artists` | ❌/⚠️ | нет | `deprecated: true` в схеме. **Не путать с фев-2026**: этот эндпоинт (вместе с `/recommendations`, `/audio-features`, `/audio-analysis`) был закрыт для всех уровней доступа ещё в ноябре 2024, задолго до текущей волны изменений — упоминаем для полноты, это не новая проблема |
| `GET /tracks/{id}` | ✅ | нет | Жив |
| `GET /tracks` (batch) | ❌ | нет | **Подтверждено live**. `deprecated: true` |
| `GET /shows/{id}`, `/episodes/{id}`, `/audiobooks/{id}`, `/chapters/{id}` | ✅ | зависит (episodes/shows: `user-read-playback-position` для части полей) | Единичные версии живы |
| `GET /shows`, `/episodes`, `/audiobooks`, `/chapters` (batch) | ❌ | — | Все batch-версии `deprecated: true`, по аналогии с albums/artists/tracks — считаем удалёнными для Dev Mode, отдельно не тестировали живьём |
| `GET /recommendations`, `/recommendations/available-genre-seeds` | ❌ | — | `deprecated: true`. Как и related-artists — закрыто с ноября 2024, не связано с февралём 2026 |
| `GET /audio-features`, `/audio-features/{id}`, `/audio-analysis/{id}` | ❌ | — | `deprecated: true`, закрыто с ноября 2024 |

### Playlists

| Эндпоинт | Статус | Scope | Заметки |
|---|---|---|---|
| `GET /playlists/{id}` | ✅ | нет (для приватных — нужен токен владельца) | Жив. Embedded-поле `tracks` → см. "Известные ловушки" |
| `GET /playlists/{id}/tracks` | ❌ | `playlist-read-private` | `deprecated: true`, в описании прямо "Use Get Playlist Items instead". **Подтверждено live** как нерабочий/пустой |
| `GET /playlists/{id}/items` | ⚠️ | `playlist-read-private` | Новый эндпоинт-замена. **Официально задокументированное ограничение** (не только наша догадка по live-тесту): "This endpoint is only accessible for playlists owned by the current user or playlists the user is a collaborator of. A `403 Forbidden` status code will be returned if the user is neither the owner nor a collaborator" — цитата из самой OpenAPI-схемы Spotify |
| `POST/PUT/DELETE /playlists/{id}/tracks` | ❌ | `playlist-modify-*` | Деприкейтед, замена — `/items` |
| `POST/PUT/DELETE /playlists/{id}/items` | ✅ | `playlist-modify-public`/`playlist-modify-private` | Новые эндпоинты. Тело запроса на DELETE изменилось: массив теперь называется `items`, а не `tracks` (`{ "items": [{ "uri": ... }] }`) |
| `GET /me/playlists` | ✅ | `playlist-read-private` | Жив, не деприкейтед |
| `POST /me/playlists` (create) | ✅ | `playlist-modify-public`/`-private` | Жив — это НЕ то же самое, что удалённый `POST /users/{id}/playlists` (см. ниже) |
| `POST /users/{user_id}/playlists` | ❌ | — | `deprecated: true`, "Use Create Playlist instead" (т.е. `POST /me/playlists`) — создание плейлиста как таковое **не удалено**, удалена только версия по чужому `user_id` |
| `GET /users/{user_id}` | ❌ | — | `deprecated: true`. Замены нет для чужих профилей — доступен только `GET /me` |
| `GET /users/{user_id}/playlists` | ❌ | — | `deprecated: true`. Замены для чужих плейлистов нет — только `GET /me/playlists` |
| `PUT/DELETE /playlists/{id}/followers` (follow/unfollow playlist) | ❌ | `playlist-modify-*` | `deprecated: true`, "Use Save/Remove Items to/from Library instead" → см. `/me/library` ниже |
| `GET /playlists/{id}/followers/contains` | ❌ | — | `deprecated: true`, замена — `/me/library/contains` |
| `GET/PUT /playlists/{id}/images` (cover) | ✅ | `ugc-image-upload`+`playlist-modify-*` (для PUT) | Не затронуто |
| `GET /browse/featured-playlists` | ❌ | нет | `deprecated: true` |
| `GET /browse/categories`, `/browse/categories/{id}`, `/browse/categories/{id}/playlists` | ❌ | нет | Все `deprecated: true` |
| `GET /browse/new-releases` | ❌ | нет | **Подтверждено live**. `deprecated: true` в схеме, но на сайте документации помечен просто "Deprecated" без формулировки замены |

### Library ("сердечко") — НЕ удалено без замены, как считалось раньше — важная находка

| Эндпоинт | Статус | Scope | Заметки |
|---|---|---|---|
| `PUT /me/library` | ✅ | один из: `user-library-modify`, `user-follow-modify`, `playlist-modify-public` | **Новый унифицированный эндпоинт** — заменяет `PUT /me/tracks`, `/me/albums`, `/me/shows`, `/me/episodes`, `/me/audiobooks`, `/me/following`, `PUT /playlists/{id}/followers`. Принимает `uris` (Spotify URI, не голые ID!) через query-параметр, максимум 40 за раз, поддерживает `track`/`album`/`episode`/`show`/`audiobook`/`user`/`playlist` |
| `DELETE /me/library` | ✅ | так же | Аналогично, заменяет все DELETE-варианты выше |
| `GET /me/library/contains` | ✅ | один из: `user-library-read`, `user-follow-read`, `playlist-read-private` | Заменяет все `*/contains` эндпоинты (tracks/albums/shows/episodes/audiobooks/following/playlist-followers), плюс поддерживает `artist` (чего не было у старых contains-эндпоинтов для треков) |
| `PUT/DELETE /me/tracks`, `/me/albums`, `/me/shows`, `/me/episodes`, `/me/audiobooks` | ⚠️/❌ | `user-library-modify` | Все `deprecated: true` с явной пометкой "Use Save/Remove Items to/from Library instead" — **замена есть**, это не тупик. Контекст задачи ошибочно считал, что замены нет вообще — на самом деле есть, просто это универсальный `/me/library` по URI, а не по голым ID |
| `GET /me/tracks/contains`, `/me/albums/contains` и т.д. | ⚠️ | `user-library-read` | Аналогично — `deprecated: true`, замена `/me/library/contains` |
| `GET /me/tracks` (читать сохранённые треки) | ✅ | `user-library-read` | **Не деприкейтед, без изменений** — как и предполагалось в задаче. Продолжает возвращать старое поле `track` (не `item`) в объектах `SavedTrackObject` — в отличие от плейлистов, здесь переименования не было |
| `GET /me/albums`, `/me/shows`, `/me/episodes`, `/me/audiobooks` (читать библиотеку) | ✅ | `user-library-read` | Не деприкейтед, живо |
| `PUT/DELETE /me/following` (follow/unfollow artist/user) | ❌ | `user-follow-modify` | `deprecated: true`, замена `/me/library` |
| `GET /me/following` (список подписок на артистов) | ✅ | `user-follow-read` | Жив, не деприкейтед |
| `GET /me/following/contains` | ❌ | `user-follow-read` | `deprecated: true`, замена `/me/library/contains` |

**Важно для миграции нашего кода**: если раньше вызывался `PUT /me/tracks?ids=...`, новый вызов — `PUT /me/library?uris=spotify:track:...` (обязательно полный URI, не голый ID, и лимит 40 не 50).

### Users / Профиль

| Эндпоинт | Статус | Scope | Заметки |
|---|---|---|---|
| `GET /me` | ✅ | `user-read-private`+`user-read-email` | Жив, без изменений структуры вызова |
| Поля `PrivateUserObject` (ответ `/me`): `country`, `email`, `explicit_content`, `followers`, `product` | ⚠️ | — | В схеме помечены `deprecated: true`, но **не удалены из схемы** — судя по формальному контракту, поля продолжают присутствовать в ответе. Это расходится с формулировкой февральского changelog, который перечисляет их как "Removed Fields" для User — возможно, February changelog описывает более раннюю/более жёсткую версию, чем то, что реально осталось в проде, либо `deprecated` тут означает "могут быть убраны позже, но пока есть". **Проверить эмпирически** и не полагаться слепо ни на changelog, ни на схему по отдельности |
| `account_id` (новое поле в `PrivateUserObject`, добавлено май 2026) | ✅ | — | "A public, immutable, pseudoanonymous identifier for the user's account" — Spotify явно рекомендует использовать его вместо `id` для связывания аккаунтов, т.к. `id` теперь помечен как нестабильный ("Do not use this field for account linking — use `account_id` instead") |
| `GET /users/{user_id}` (чужой публичный профиль) | ❌ | — | См. таблицу Playlists выше |
| `GET /me/top/{type}` (топ артистов/треков пользователя) | ✅ | `user-top-read` | Не затронуто, живо |

## Известные ловушки (fail silently, не бросают явную ошибку)

Эти три — самые опасные, потому что не кидают исключение, а просто тихо возвращают "не то" или "пусто":

1. **`.track` → `.item` в объектах плейлиста.** В ответе `GET /playlists/{id}` и `GET /playlists/{id}/items`, поле `PlaylistTrackObject.track` (объект трека/эпизода внутри пункта плейлиста) переименовано в `PlaylistTrackObject.item`. Старое `track` оставлено в схеме как deprecated alias, но по факту (наш опыт) оно возвращает пустое/ненадёжное значение — **не полагаться на него**. То же самое верх по иерархии: `PlaylistObject.tracks` (пагинированный список) → `PlaylistObject.items`, с тем же deprecated-alias поведением у `tracks`. Итого путь к треку внутри пункта плейлиста меняется с `playlist.tracks.items[i].track` на `playlist.items.items[i].item`.
   - Важная деталь, которую легко упустить: поле `items` (и в объекте плейлиста, и как отдельный эндпоинт) **в принципе отсутствует/403**, если пользователь не владелец и не коллаборатор плейлиста — это официально задокументированное поведение, не баг с нашей стороны.
   - `GET /me/tracks` (сохранённые треки, НЕ плейлист) этого переименования не претерпел — там как был, так и остался `SavedTrackObject.track`. Не путать два похожих, но по-разному ведущих себя эндпоинта.

2. **`GET /search` — лимит.** Максимум `limit` срезан с 50 до **10**, дефолт — с 20 до **5** (per item type, т.е. если ищете `type=album,track`, лимит применяется к каждому типу отдельно, не суммарно). Если в коде раньше был захардкожен `limit=20` или `limit=50` — теперь это `400 Bad Request` вместо тихого клампа, что хотя бы не тихо, но легко пропустить при беглом тестировании, если раньше запрос всегда укладывался в дефолт.

3. **`id` пользователя не стабилен для связывания аккаунтов.** С мая 2026 в `PrivateUserObject` появилось поле `account_id`, и документация прямо говорит не использовать `id` для account linking. Если наш код где-то хранит Spotify `id` пользователя как первичный ключ для связывания с локальной учёткой — стоит смигрировать на `account_id`.

4. **`GET /artists/{id}/albums` — лимит.** Максимум `limit` теперь **10** (раньше — 50). Не упомянуто ни в одном найденном changelog явно (проверено по факту в текущей OpenAPI-схеме) — если код запрашивает дискографию артиста постранично с прежним шагом в 50, придётся пагинировать чаще.

5. **Deprecated ≠ Removed.** Общая ловушка при чтении документации Spotify: пометка "Deprecated" на странице/в схеме означает "используйте новый эндпоинт", но НЕ гарантирует немедленную поломку — например, `PUT /me/tracks` в схеме просто deprecated и (по нашим ограниченным наблюдениям на момент фев-2026 миграции) какое-то время продолжал отвечать `200`, пока февральский changelog для Dev Mode не объявил его "removed". Поведение "живо, но deprecated" и "уже жёстко удалено (403/404)" в документации выглядят одинаково (один и тот же флаг) — различать их можно только явным live-тестом или по формулировке в monthly changelog ("Removed Endpoints" vs просто описание с "Use X instead").

## Что уже подтверждено нашим собственным live-тестированием (не только по документации)

- `GET /artists/{id}/top-tracks` → 403
- `GET /albums` (batch) → 403
- `GET /playlists/{id}/tracks` → 403/пусто
- `GET /search` с `limit>10` → ошибка (было тихо ограничено раньше)
- `PUT/DELETE/GET /me/tracks/*` — поведение изменилось при миграции (см. важное уточнение выше: замена `/me/library` существует, хотя изначально казалось, что её нет)

Всё остальное в таблицах выше — из официальной OpenAPI-схемы и changelog, живьём не перепроверялось; там где это существенно, помечено ❓/⚠️ с пояснением, что именно неясно.
