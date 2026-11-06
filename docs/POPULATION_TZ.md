# ТЗ: популяции стратегий в backtest venue

Техническое задание по итогам дизайн-сессии.  
Смежные документы:

- **план работ (писать код)** — [`POPULATION_IMPL.md`](POPULATION_IMPL.md)
- черновик архитектуры — [`POPULATION_PLAN.md`](POPULATION_PLAN.md)
- глоссарий — [`GLOSSARY.md`](GLOSSARY.md)
- CMF — [`TASK.md`](TASK.md)
- контекст репо — [`CONTEXT.md`](CONTEXT.md)

**Статус:** согласованный каркас + явный список открытых пунктов (§8).  
**Не в этом документе:** построчный код, калибровка `λ`, замена JSONL на бинарный формат.

---

## 1. Цель

Venue обслуживает **популяции** стратегий на **общей ленте** (один `pop_ts`),
при этом у каждой стратегии **свой** effective book на **каждую** ногу bundle
(свои resting + свой impact). Логику геномов / популяций ведёт **Trading Engine**;
связь с venue — **unix domain socket** (создание очередей) + **две SPSC** на
прогон (весь протокол), **pull** по одному тику.

**Книги популяции (канон):** до **трёх** компонент — **spot**, **futures**,
**option**. Если какой-то ноги нет в датасете / Open — считаем **меньше**
(1 или 2); не подставляем пустышку «ради тройки». Один `pop_ts`, merge лент
по `ts_ns` (§6).

**Сейчас в коде** (`Population`) — ещё **одна** книга; это отставание
реализации, не смена договорённости.

**gRPC снят с канона:** отдельный runtime и лишняя стоимость на горячем пути.

CMF TCP (`feed` + `orders`) остаётся 1-slot smoke / legacy engine, **не**
мультиплекс популяций.

---

## 2. Роли и границы

| Роль | Процесс | Ответственность |
| --- | --- | --- |
| **Venue** | `backtest` | Датасет, TapeWindow, матч, impact, working orders, Exec/Trades; слушает UDS |
| **Trading Engine** | внешний процесс | Подключается к UDS, открывает популяцию, код стратегий, учёт филлов / маржи / PnL |
| **Strategy (геном)** | внутри Trading Engine | Решение по view; помнит в основном **сделки**; working book не дублирует |

Стратегии **не конкурируют** за книгу и **не видят** чужие ордера/филлы.

---

## 3. Модель времени и видимости (канон D)

Один clock на популяцию: **`pop_ts`**.

На шаге `t`:

```text
view_ts_i      = t - md_lag_i     # что видит слот i
t_seen         = t                # момент решения
effective_ts_i = t_seen + Δ_i     # когда новый ордер matchable
```

| Параметр | Где | Смысл |
| --- | --- | --- |
| **`md_lag`** | per-slot | Устаревание MD (сеть + пересчёт). View = effective book на `t - md_lag` |
| **`Δ`** | per-slot | Задержка ордерного канала до матчера |
| **Модель B** | частный случай D | `md_lag = 0`, `Δ` как задано (ближе к CMF/live view) |

Нет look-ahead: стратегия не видит ts > `t - md_lag`.

**Снято с канона (относительно старого черновика плана):** разный `virtual_ts` у
слотов, `HorizonOffset`, leader `argmax(offset)`, catch-up догон внутри
популяции. Разница во времени между стратегиями = только `md_lag` и `Δ`.

---

## 4. Лента и память (одна на популяцию, книг — до трёх)

Один `pop_ts` и одно окно времени на популяцию; внутри — **до трёх** file-книг
(spot / futures / option), у каждой свой `TapeWindow` / cursor по своему JSONL.
Слот держит **ImpactState + Working на каждую присутствующую** ногу.

```text
SnapshotFloor = pop_ts - max_i(md_lag_i)   # хватает истории для самого «тупого» view
TapeWindow_c  = snap@floor ⊕ BookDelta… ⊕ … до pop_ts   # per component_id
```

- Bundle (1…3 ноги) мержится в общий timeline по `ts_ns`; читается **один раз**
  в окна компонент.
- **BookDelta** = патч шага ленты **одной** ноги (100→101). Не называть «offset».
- Нога без события на этом `t` → **keep last** её file-book.
- При **Advance**: `pop_ts` += шаг; полы окон ползут; префиксы можно дропнуть.
- Слот **не** копирует file-book; только `ImpactState` + Working **per component**.

Effective book слота × ноги:

```text
effective_qty[c] = max(0, file_qty[c] - eaten_slot[c])
+ resting orders этого слота по component_id c
```

**Impact:** per-slot × component `eaten` + экспоненциальное восстановление при
живом уровне в файле этой ноги; уровень исчез → сброс eaten. **`λ` — per-slot**
(дефолт с популяции): `λ = 0` выключает impact; `λ > 0` включает recovery.

---

## 5. Транспорт: Unix socket + SPSC

**Инвариант:** логика протокола (§5.3–5.4, state machine, типы сообщений)
**та же**, что была задумана для stream. Меняется только носитель:
всё общение прогона — **через пару очередей**; отдельно — канал, который
эти очереди **создаёт**.

### 5.1 Топология

| Решение | Выбор |
| --- | --- |
| Канал создания | **Unix domain socket**: Engine dial’ит venue → «создай пару очередей» |
| Общение прогона | **Две SPSC** — весь протокол (Open → … → End/Close) только здесь |
| Кто ведёт часы | **Pull:** Engine; venue сам тики не пушит |
| Шаг | **Последовательно каждый тик** (или будущий coarse stride) |
| CMF TCP | Только 1-slot smoke |
| gRPC | **Не используем** (runtime / лишняя стоимость) |

```text
Trading Engine ── unix socket ──► Venue
   │     create 2×SPSC | reject          # до прогона
   │     SessionEnded { ok | fatal }     # после прогона (норма / критика)
   │
   └── SPSC out / in ── протокол тиков  # Open…End/Error/Close
```

**Квоты (позже):** нет свободной квоты → отказ на UDS, SPSC не поднимаем.
Квоты **не в P0** — контракт «успех создания = две очереди».

UDS **живёт на весь прогон**: не только create, но и финальный
`SessionEnded` (§5.5).

Зачем pull: engine может не успевать считать популяцию; без Advance рынок не
убегает, старые снепшоты не дропаются раньше времени.

Зачем один тик за round-trip: видна динамика исполнения; можно докинуть ордера
на следующем тике. Немой `Advance(+N)` без промежуточных view/trades — **не
делаем**.

### 5.2 Канал создания vs очереди

1. Engine dial’ит **unix socket** и просит **создать пару SPSC** (здесь же —
   проверка квоты, когда появится). Ответ: handles/имена очередей или отказ.
2. Дальше **вся** протокольная переписка — **только по очередям**:
   - **out** (venue → engine): OpenAck, Tick, OrdersAck, End, Error;
   - **in** (engine → venue): Open, OrderBatch, Advance, Close.
3. Первое сообщение в in после создания = **`Open`** (`population_id` + слоты /
   dataset / components) → популяция фиксируется; mid-run конфиг не меняем.
4. Цикл §5.4 по SPSC, пока лента жива; нормальный конец — **`End`** в out.
5. **`Close`** в in — graceful shutdown (досрочный снос прогона).

UDS **не** несёт Tick/Orders — только create/reject и финальный `SessionEnded`
(§5.5). Teardown очередей — после `SessionEnded` и drain/timeout.

In-process callback API **пока не делаем**.

### 5.3 Типы событий (логическая схема; имена как в proto-черновике)

Фрейминг байтов в SPSC — отдельный шаг; **gRPC не цель**.
Логические типы и state machine **без изменений по смыслу**.

| Когда | Направление | Тип | Канал | Содержимое |
| --- | --- | --- | --- | --- |
| Подготовка | Engine ↔ Venue | create queues | UDS | handles 2×SPSC или отказ (квота) |
| Финал | Venue → Engine | **`SessionEnded`** | UDS | итог прогона: ok / fatal (+ code); см. §5.5 |
| Старт | Engine → Venue | `Open` | SPSC in | конфиг популяции |
| Старт | Venue → Engine | **`OpenAck`** | SPSC out | всегда; `status` + при ERROR — `errors[]` |
| Каждый тик | Venue → Engine | `Tick` | SPSC out | books + trades; затем ждём клиента |
| После Tick | Engine → Venue | `OrderBatch` **или** `Advance` | SPSC in | другого не ждём |
| Если OrderBatch | Venue → Engine | **`OrdersAck`** | SPSC out | всегда; `status` + при ERROR — `errors[]` |
| После ok Ack | Engine → Venue | `Advance` | SPSC in | затем venue → следующий Tick \| End |
| Конец ленты | Venue → Engine | `End` | SPSC out | **без ack** — закрыть прогон |
| Graceful stop | Engine → Venue | `Close` | SPSC in | **без ack** в любом клиентском стейте |
| Сбой протокола | Venue → Engine | `Error` | SPSC out | неверный стейт (см. ниже) |

**Почему Ack, а не bare Error:** после `Open` / `OrderBatch` клиент всегда ждёт
один тип (`OpenAck` / `OrdersAck`). Сначала смотрит **`status`**:
`ACK_STATUS_OK` → всё хорошо, `errors` пуст; `ACK_STATUS_ERROR` → смотреть
`errors[]`.

- **`OrdersAck` ERROR:** wire/схема — на потом (**recoverable ack**): all-or-nothing,
  retry batch без сноса. Сейчас битый batch → fatal §5.5.
- **`OpenAck` ERROR:** сломанный конфиг → fatal §5.5 (ждать Close | timeout).
  Отказ по **квоте** — ещё на UDS, до выдачи SPSC.

Филлы — в `Tick.trades`.

`End` / `Close` / фатальный `Error` на SPSC — сигнал **стратегическому** циклу;
официальное закрытие сессии для Engine — всегда **`SessionEnded` на UDS** (§5.5).

### 5.5 Завершение и ошибки (UDS + SPSC)

| Класс | Когда | Куда | Поведение |
| --- | --- | --- | --- |
| **Create reject** | квота / нельзя поднять SPSC / битый create | **только UDS** | очередей нет |
| **Fatal run** | `OpenAck` ERROR, protocol `Error`, авария venue | SPSC `Error` (best-effort) + UDS `SessionEnded{fatal}` | стратегия **не** чинит; ждём `Close` или timeout |
| **Normal end** | лента кончилась | SPSC `End` + UDS `SessionEnded{ok}` | штатный выход |
| **Client Close** | Engine сам закрыл | UDS `SessionEnded{ok, CLIENT_CLOSE}` | без критики |
| **Recoverable ack** | плохой `OrderBatch`, retry без сноса | `OrdersAck` ERROR | **позже** (§8.2); сейчас не опираемся |

**Fatal: ожидаем `Close` от Engine.** Стратегия читает Error из SPSC → шлёт
`Close` (подтверждение «увидели, гасим»). Venue после `Close` → teardown.

**Если `Close` не пришёл за `drain_timeout` (default 60s):** venue **сам**
аварийно завершает сессию: кратко пишет в `SessionEnded`, *что* сломалось
(`reason_code` + короткий `message`), сносит SPSC, закрывает UDS. Без этого
зависшие / нечитающие клиенты держат квоту вечно.

**Почему ещё и UDS.** SPSC может быть полной / engine не в цикле — Error в
очереди best-effort. `SessionEnded` на сокете — гарантированный итог; в
аварийном timeout-пути это **единственное**, что Engine потом увидит при
разборе (краткий диагноз обязателен).

**Порядок fatal teardown (venue):**

```text
1. SPSC out ← Error | OpenAck ERROR   (best-effort, если out полна — см. ниже)
2. in: принимать только Close; остальное игнор / no-op
3. UDS ← SessionEnded{fatal, reason_code, message}   # кратко: что не так
4. Ждать Close от Engine
   — пришёл → teardown (Engine подтвердил чтение)
   — drain_timeout → аварийный teardown сами (диагноз уже в SessionEnded)
5. Снести SPSC; закрыть UDS
```

**Normal end:** SPSC `End` → `SessionEnded{ok}` → ждать `Close` / timeout
так же (без «критики» в message).

Если out полна на шаге 1 — не крутиться вечно (reserved slot / drop-oldest);
истина для диагноза — в `SessionEnded`.

**Create reject** — сразу ответ на UDS, без SPSC и без wait на Close.

**Не делаем сейчас:** mid-run repair стратегии; recoverable `OrdersAck` как
основной путь (заложим wire позже); хардкод 60s без конфига.

#### State machine (venue, обязателен)

Venue хранит фазу прогона; неожиданное клиентское сообщение → **`Error`**
и обычно конец прогона.

```text
# UDS уже выдал 2×SPSC (иначе сюда не попадаем)
AwaitOpen
  Open → OpenAck; if status=OK → send Tick → AwaitOrderOrAdvance
         if status=ERROR → §5.5 fatal teardown (OpenAck уже в SPSC)

AwaitOrderOrAdvance          # только что ушёл Tick (или первый после Open)
  OrderBatch → (пока: валидация; битый batch → fatal §5.5;
                позже: OrdersAck ERROR + retry без сноса)
       ok → AwaitAdvance
  Advance  → apply; send Tick | End
       Tick → AwaitOrderOrAdvance
       End  → §5.5 normal (SessionEnded ok → ждать Close | timeout)
  Close    → §5.5 SessionEnded{ok, CLIENT_CLOSE}
  иное (повторный Open, …) → Error → §5.5 fatal (ждать Close | timeout)

AwaitAdvance
  Advance  → apply; send Tick | End → (Tick → AwaitOrderOrAdvance | §5.5 normal)
  Close    → §5.5 SessionEnded{ok, CLIENT_CLOSE}
  OrderBatch / Open / … → Error → §5.5 fatal (ждать Close | timeout)

Запрещено (примеры → Error #3):
  Advance до первого Tick
  Advance, пока ждём ответ на… (нет: Advance из AwaitOrderOrAdvance разрешён —
    это «нет ордеров, двигай время»)
  OrderBatch до Tick / во второй раз подряд без Advance после ok Ack
```

**Итого после Tick:** клиент шлёт либо `OrderBatch` (потом Ack и Advance),
либо сразу `Advance` (пустой интерес к ордерам). `Advance` «в пустоту» до
Tick — ошибка.

Стратегия для маржи/оборота/PnL — по полю **trades**.
Working и ImpactState в venue. На wire:

- **`Open.components`**: **1…3** ноги типичного набора (SPOT / FUTURE / OPTION);
  у каждой уникальный **`component_id`**, `role`, JSONL + class/sec/tick.
  Нет ноги в Open → её нет в Tick (не шлём пустой book «для галочки»).
  Несколько разных future/option (expiries) — допустимо как расширение; канон
  прогона = спот + фьюч + опцион или подмножество.
- **`Tick`**: books / overlays / trades **по `component_id`** для всех ног,
  которые уже дали первое событие.
- **`Tick.books`** (shared seed): на первом тике при одинаковом view; при
  per-slot `md_lag` — патчи в **`SlotTick.books`** (Keyframe → Diff).
- Order/Trade с `component_id`. Один `Δ` на слот (не три HELLO).

**Реализация сейчас:** один component — временно; цель — довести до 1…3.

### 5.4 Цикл одного тика (логический) — канон

`Δ = 0` **запрещён** (ошибка конфига / Open / слота). Минимальный легальный
`Δ` пока **1** (логическая единица времени). Шаг ленты задаёт не `step_ns`,
а события датасета и **`tape_stride`** (сколько событий на один Advance).

На одном `t` venue проходит **все** слоты одинаково (нет гонки «кто быстрее»
между стратегиями):

```text
1. apply file delta на t                          # общая лента
2. recovery impact по каждому слоту
3. match due по каждому слоту независимо
   (effective_ts ≤ t; resting в порядке отправки в слоте)
4. Venue → Engine Tick (SPSC out): books + trades
5. Engine → Venue: либо OrderBatch → OrdersAck → Advance, либо сразу Advance
6. Advance: pop_ts → следующий event; SnapshotFloor; venue сам шлёт Tick|End
```

**Изоляция:** слоты не видят чужие филлы/ордера и не конкурируют за
ликвидность; матч A не меняет effective B. «Кто быстрее отправил» между
стратегиями не считается — только порядок заявок **внутри** одного слота.

Новый ордер на `t` всегда с `effective_ts > t`. Филлы — в `trades` того Tick,
на чьём `t` сработал due-матч.

---

## 6. ComponentBundle (spot / futures / option)

**Договорённость:** популяция считает **до трёх** книг — спот, фьючерс, опцион.
Меньше — нормально, если какой-то ноги нет. Больше трёх ролей в одном прогоне
не цель P0–P1 (несколько expiry одной роли — отдельное расширение).

На один `pop_ts` — merge JSONL ног по `ts_ns`. Нет данных роли → роли нет в
Open / нет в Tick. В `Tick` — effective books **всех присутствующих** ног слота
на его `view_ts` (кодирование — §8.3, §8.5).

Один lookahead/`Δ` на слот, не три разных HELLO на инструмент.

---

## 7. Фазы реализации

| Фаза | Содержание | Готово, когда |
| --- | --- | --- |
| **P0** | Одна популяция; K слотов; impact `λ = 0`; UDS + 2×SPSC; **пока 1 JSONL** ок как подмножество bundle | Детерминированный повтор; pull не обгоняет Engine |
| **P1** | Impact + per-slot `λ`; Match по effective; Working per-slot; trades в Tick; CMF 1-slot зелёный | Филл только у своего слота; recovery; `make run` CMF OK |
| **P1b** | **Обязательный** ComponentBundle: spot + futures + option (или меньше, если нет ноги); merge по `ts_ns` | Один `pop_ts`; в Tick books всех присутствующих ног |
| **Index** | Sidecar `ts → file_offset` / keyframes (старт всё равно с начала файла) | Быстрее читать длинный JSONL |
| **P2** | Multi-population + **квоты**; catalog датасетов; perf (stride и т.д.) | 2+ популяции стабильно; отказ Open при нет квоты |

### Модули кода (ориентир)

| Модуль | Ответственность |
| --- | --- |
| `dataset/` | Bundle, readers, merge, index seek |
| `book/` | Shared file book, deltas, tick math |
| `impact/` | eaten + recovery |
| `match/` | MatchOne vs effective view |
| `population/` | slots, pop_ts, TapeWindow, Advance |
| `transport/uds/` | Unix socket listen; Open / OpenAck; (позже) квоты |
| `transport/spsc/` | Пара очередей на прогон: venue→engine data, engine→venue requests |
| `transport/cmf/` | 1-slot adapter |

---

## 8. Осталось рассмотреть

Ниже — открытый frontier. Пока нет ответа, в реализации брать явный TODO / не
зашивать молча.

### 8.1 Порядок внутри тика — **закрыто**

Канон = §5.4 (вариант 1): file → recovery → match due → Tick → Order → Advance(PS).

- `Δ = 0` — reject, не «матч в том же тике».
- `trades` в Tick = только филлы due-матча **до** выдачи view (п.3).
- Live CMF-порядок (due по старому book → overwrite → rematch) **не** канон
  популяции.

### 8.2 Control API детали

**Закрыто:** open = `population_id` + параметры → фиксация; mid-run
конфиг/слоты не меняем (§5.2). Транспорт = **UDS control + 2×SPSC data**;
gRPC не делаем.

**Идентификатор ордера:** отдельного venue-`order_id` **нет**. Ключ =
`(slot_id, client_order_id)`. Клиент задаёт `client_order_id` на `Order`; все
ack / partial / fill / cancel / reject — по нему. `Cancel` несёт **тот же**
`client_order_id`, что был на `Order`. После терминального состояния id мёртв;
новый ордер — только с новым id. Уникальность в пределах слота — обязанность
Engine; venue reject’ит дубликат живого id.

**Закрыто по lifecycle (§5.5):** UDS живёт весь прогон; create + `SessionEnded`;
fatal → Error в SPSC → ждать **`Close`** (стратегия прочитала) иначе
**timeout → сами аварийно** с кратким `message` что сломалось; normal End —
тот же wait на Close.

**Ещё открыто / позже:**
- **recoverable `OrdersAck`** — retry batch без сноса прогона (wire можно
  оставить в proto, поведение — не сейчас);
- байтовый фрейминг create/`SessionEnded` / SPSC;
- handles SPSC (path / fd / shm name);
- reserved slot vs drop-oldest при полной out;
- **квоты** — позже.

### 8.3 Кодирование book на wire — **закрыто**

- Bundle в Open: до 3 `Component` (spot / futures / option или меньше);
  уникальный `component_id`; `role` = класс.
- **`Tick.books`**: shared keyframe/diff **на component_id**; не копировать в слоты.
- **`overlays`**: per-slot × component_id; абсолютный qty; `0` снимает уровень.
- `trades` / Order — с `component_id`.
- Лимит уровней: **64** на сторону (как CMF), пока не решим иначе.

### 8.4 Impact на `view_ts`

View = effective на `t - md_lag`. Нужен ли **исторический** eaten на view_ts
(как будто recovery крутился в прошлом), или упрощение: текущий eaten, вычтенный
из **старого** file snap? Влияет на честность md_lag.

### 8.5 Bundle / выравнивание — **закрыто**

- **Не** требуем оффлайн одну ценовую/временную сетку на все JSONL
  (дорого). Venue мержит по `ts_ns` на общем `pop_ts`.
- Компонент без события на этом `t`: **keep last** book этой роли.
- Разный старт файлов: роли без данных ещё не появляются в `Tick.books`
  / пустой view этой роли, пока не придёт первое событие (не ждём
  искусственного align всех трёх).
- На wire: один `Tick`, внутри `books[]` / overlays **по `component_id`**.

### 8.6 Индекс и старт

**Старт прогона:** всегда с **первого** события датасета (mid-file seek нет;
`ts_start` в Open убран).

Индекс (если понадобится позже) — для ускорения чтения/keyframes, не для
старта с середины дня в текущем каноне.

### 8.7 Семантика TIF / очередь — **закрыто**

- **TIF:** DAY (rest leftover), IOC (match → cancel leftover), FOK (all-or-nothing
  иначе reject) — как в текущем CMF venue.
- **Очередь:** внутри слота — порядок отправки стратегией; между слотами
  конкуренции нет (§5.4).

### 8.8 Multi-population fairness

- Несколько прогонов (пар SPSC) в одном процессе venue: thread-per-population? пул?
- CPU quantum между популяциями.
- Квоты на число одновременных Open (см. §5.1) — отдельный рычаг.

### 8.9 Ускорение (не P0)

- `tape_stride` в Open уже есть: P0 держать `1`; `N > 1` — склейка N событий
  файла в один Tick (обычно last book), один round-trip на склейку.
- Бинарный датасет вместо JSONL на горячем пути.

### 8.10 Транспортный каркас — **закрыто (семантика)**

UDS Open → OpenAck (+ 2×SPSC при OK); цикл Tick → Orders → Advance по SPSC;
нормальный конец = venue `End`; опционально клиент `Close` = graceful
shutdown only. In-process API не в P0. **gRPC не входит в план.**

Имена/схема сообщений могут жить в `protos/` как IDL без gRPC; wire =
собственный фрейминг поверх UDS/SPSC (§8.2).

### 8.11 Документы

- Синхронизировать [`POPULATION_PLAN.md`](POPULATION_PLAN.md) /
  [`GLOSSARY.md`](GLOSSARY.md) с этим ТЗ (транспорт UDS+SPSC).
- ADR на «один `pop_ts` + UDS/SPSC pull + model D» — по запросу.

---

## 9. Зафиксированные default’ы (шпаргалка)

| Вопрос | Default |
| --- | --- |
| Конкуренция слотов за книгу | Нет; свой effective + impact |
| Часы популяции | Один `pop_ts` |
| Видимость | Model D (`md_lag` + `Δ`); B = `md_lag=0` |
| Лента | SnapshotFloor + BookDelta window |
| Кто двигает время | Trading Engine (pull Advance) |
| Шаг | Каждый тик, со стримом динамики |
| Транспорт | UDS: create + SessionEnded; тики по 2×SPSC; fatal → Error → ждать Close иначе timeout+краткий диагноз; recoverable OrdersAck — позже |
| `tape_stride` | Склейка N событий файла в один Tick; P0 = 1; не путать с price `tick_size` |
| Старт ленты | Всегда первое событие файла; mid-file seek нет |
| View на wire | books/overlays keyed by `component_id` + trades |
| Bundle | До 3 ног: spot + futures + option (или меньше); `component_id` уникален; нет ноги → нет в Tick; код сейчас ещё 1 книга |
| Выравнивание лент | Merge по `ts_ns` + keep last; без оффлайн-решётки |
| Open / mid-run | `population_id`+params → fix; конфиг mid-run не трогаем |
| Лимит L2 | 64 уровня на сторону |
| Id ордера | `(slot_id, client_order_id)`; venue-`order_id` нет; Cancel = тот же id |
| Resting priority в слоте | Порядок отправки стратегией (раньше → раньше в матче) |
| Между стратегиями | Нет конкуренции, нет видимости чужих филлов; Tick — батч по слотам |
| TIF | DAY / IOC / FOK; `UNSPECIFIED` → `Error` |
| Side | BUY / SELL; `UNSPECIFIED` → `Error` |
| Phase 1 host | Trading Engine dial’ит venue по UDS (не embed-only) |
| CMF | 1-slot smoke |
| Impact config | Per-slot `λ`; `λ = 0` = off |
| Немой multi-tick Advance | Нет |
| `Δ = 0` | Запрещён (ошибка) |
| Порядок тика | §5.4: file → recovery → match due → Tick → Order → Advance(PS) |
| `trades` в Tick | Филлы due-матча на этом `t`, до view |

---

## 10. Вне скоупа

- Смена CMF line-protocol для single-engine пути.
- Калибровка прод-`λ`.
- Полная замена JSONL на колонный формат (только medium-term заметка).
- Global B-tree catalog по всем датасетам (после sidecar на один файл).
