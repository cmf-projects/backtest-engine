# ТЗ: популяции стратегий в backtest venue

Техническое задание по итогам дизайн-сессии.  
Смежные документы (пока **не** переписываем под это ТЗ):

- черновик плана — [`POPULATION_PLAN.md`](POPULATION_PLAN.md)
- глоссарий — [`GLOSSARY.md`](GLOSSARY.md)
- CMF — [`TASK.md`](TASK.md)
- контекст репо — [`CONTEXT.md`](CONTEXT.md)

**Статус:** согласованный каркас + явный список открытых пунктов (§8).  
**Не в этом документе:** построчный код, калибровка `λ`, замена JSONL на бинарный формат.

---

## 1. Цель

Venue обслуживает **популяции** стратегий на **общей ленте** (один `pop_ts`, одно
окно `BookDelta`), при этом у каждой стратегии **свой** effective book (свои
resting + свой impact). Логику геномов / популяций запускает **отдельный
Population Service**; связь с venue — **gRPC**, один bidi stream на популяцию,
**pull** по одному тику.

CMF TCP (`feed` + `orders`) остаётся 1-slot smoke / legacy engine, **не**
мультиплекс популяций.

---

## 2. Роли и границы

| Роль | Процесс | Ответственность |
| --- | --- | --- |
| **Venue** | `backtest` / venue-сервис | Датасет, TapeWindow, матч, impact, working orders, Exec/Trades |
| **Population Service** | отдельный сервис | Запуск популяций, код стратегий, учёт филлов для маржи/оборота/PnL |
| **Strategy (геном)** | внутри Population Service | Решение по view; помнит в основном **сделки**; working book не дублирует |

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

## 4. Лента и память (одна на популяцию)

```text
SnapshotFloor = pop_ts - max_i(md_lag_i)   # хватает истории для самого «тупого» view
TapeWindow    = snap@floor ⊕ BookDelta… ⊕ … до pop_ts
```

- Файл/мердж bundle читается **один раз** в окно.
- **BookDelta** = патч шага ленты (100→101). Не называть «offset» (не путать с
  отставанием слота в старых черновиках).
- При **Advance**: `pop_ts` += шаг; пол ползёт; префикс окна можно дропнуть.
- Слот **не** хранит копию file-book; только `ImpactState` + Working.

Effective book слота:

```text
effective_qty = max(0, file_qty - eaten_slot)
+ resting orders этого слота в картине view
```

**Impact:** per-slot `eaten` + экспоненциальное восстановление при живом уровне в
файле; уровень исчез из файла → сброс eaten. **`λ` — per-slot**
(дефолт с популяции): `λ = 0` выключает impact; `λ > 0` включает recovery.

---

## 5. Транспорт gRPC

### 5.1 Топология

| Решение | Выбор |
| --- | --- |
| Кто ведёт часы | **Pull:** Population Service; venue сам тики не пушит |
| Шаг | **Последовательно каждый тик** (или будущий coarse stride) |
| Каналы | **Один bidi stream на популяцию** |
| Feed/orders split | **Нет** (не CMF-пара); разные **типы событий** в одном stream |
| CMF TCP | Только 1-slot smoke |

Зачем pull: сервис может не успевать считать популяцию; без Advance рынок не
убегает, старые снепшоты не дропаются раньше времени.

Зачем один тик за round-trip: видна динамика исполнения; можно докинуть ордера
на следующем тике. Немой `Advance(+N)` без промежуточных view/trades — **не
делаем**.

### 5.2 Один bidi stream (control + data)

Отдельных unary Open/Close **нет**. Один bidi stream на популяцию:

1. **Первое сообщение клиента** = control: `population_id` + параметры слотов
   / dataset → популяция **фиксируется**. Mid-run конфиг не меняем.
2. Дальше **data-цикл** (§5.4) по кругу, пока лента жива.
3. Нормальный конец: venue шлёт **`End`** (лента кончилась).
4. Опционально клиент шлёт **`Close`** — только graceful shutdown (досрочно
   свернуть прогон), не смена конфига и не control mid-run.

In-process callback API **пока не делаем** (только gRPC stream).

### 5.3 Типы событий в stream (черновик имён)

| Когда | Направление | Тип | Содержимое |
| --- | --- | --- |
| Старт (1×) | PS → Venue | `Open` (имя черновое) | `population_id`, слоты, dataset, … |
| Каждый тик | Venue → PS | `Tick` | `pop_ts`; shared books **per role** + per-slot overlay/trades |
| Каждый тик | PS → Venue | `Order` / `Cancel` | `slot_id`, `client_order_id`, …; Cancel = тот же id |
| Каждый тик | PS → Venue | `Advance` | тик обработан, можно +1 |
| Конец ленты | Venue → PS | `End` | датасет кончился |
| Ошибка | Venue → PS | `Error` | валидация/протокол (`TIF`/`SIDE` UNSPECIFIED, `Δ=0`, …) |
| Graceful stop | PS → Venue | `Close` | клиент завершает прогон досрочно; не больше чем shutdown |

**`Error` может встраиваться между любыми сообщениями stream’а**, если
предыдущее клиентское сообщение было некорректным (плохой Order/Open,
UNSPECIFIED enum, …). Это не замена Tick/End: после ошибочного Order типично
идёт `Error`, затем при валидном Advance цикл может продолжаться. Плохой
`Open` обычно → `Error` и конец stream’а.

`TIF_UNSPECIFIED` / `SIDE_UNSPECIFIED` на Order → **`Error`** (`INVALID_TIF` /
`INVALID_SIDE`), ордер не принимается.

Стратегия для маржи/оборота/PnL — по полю **trades**.
Working и ImpactState в venue. На wire:

- **`Open.components`**: 1…N ног; у каждой уникальный **`component_id`**,
  `role` (SPOT/FUTURE/OPTION — класс, не ключ), JSONL + class/sec/tick.
  Несколько future/option — несколько Component с разными id.
- **`Tick.books`**: общий keyframe **только на первом тике** (все стратегии
  сидят с одного seed).
- Дальше `Tick.books` пустой; патчи → **`SlotTick.books`** (Diff обычный;
  Keyframe только этому слоту, если diff дорогой **или** стратегия сбросила
  локальный стакан и прислала `ResyncRequest`).
- Order/Trade с `component_id`. Один `Δ` на слот.

P0 может открыть bundle из **одного** компонента.

### 5.4 Цикл одного тика (логический) — канон

`Δ = 0` **запрещён** (ошибка конфига / Open / слота). Минимальный легальный
`Δ` пока **1** (логическая единица времени). Шаг ленты задаёт не `step_ns`,
а события датасета и **`tape_stride`** (сколько событий на один Advance).

На одном `t` venue проходит **все** слоты одинаково (нет гонки «кто быстрее»
между стратегиями):

```text
1. apply file delta на t                          # общая лента
2. recovery impact по каждому слоту (P1; P0 no-op)
3. match due по каждому слоту независимо
   (effective_ts ≤ t; resting в порядке отправки в слоте)
4. Venue → PS одним Tick: shared_book + по слотам overlay/trades
5. PS считает каждую стратегию → пачка Order/Cancel по слотам
   Venue только принимает (effective_ts = t + Δ_i), без матча
6. PS → Advance
7. Advance: pop_ts → ts следующего (после tape_stride) события; SnapshotFloor
```

**Изоляция:** слоты не видят чужие филлы/ордера и не конкурируют за
ликвидность; матч A не меняет effective B. «Кто быстрее отправил» между
стратегиями не считается — только порядок заявок **внутри** одного слота.

Новый ордер на `t` всегда с `effective_ts > t`. Филлы — в `trades` того Tick,
на чьём `t` сработал due-матч.

---

## 6. ComponentBundle

На один `pop_ts` — 1…N компонент (типично spot + future + option), merge по
`ts_ns`. Нет данных роли → роли нет в манифесте. В `Tick` — effective books
**всех** ролей слота на его `view_ts` (детали кодирования — §8).

Один lookahead/`Δ` на слот, не три разных HELLO на инструмент.

---

## 7. Фазы реализации

| Фаза | Содержание | Готово, когда |
| --- | --- | --- |
| **P0** | Одна популяция, один JSONL; TapeWindow + BookDelta (или временно полные snap в окне); K слотов; impact с `λ = 0`; gRPC Tick/Order/Advance | Детерминированный повтор прогона; pull не обгоняет PS |
| **P1** | Impact + per-slot `λ`; Match по effective; Working per-slot; trades в Tick; CMF 1-slot зелёный | Филл только у своего слота; recovery; `make run` CMF OK |
| **P1b** | ComponentBundle merge fut/spot/opt | Один `pop_ts`, несколько books в Tick |
| **Index** | Sidecar `ts → file_offset` / keyframes (старт всё равно с начала файла) | Быстрее читать длинный JSONL |
| **P2** | Multi-population hardening, catalog датасетов, perf (stride и т.д.) | 2+ популяции стабильно |

### Модули кода (ориентир)

| Модуль | Ответственность |
| --- | --- |
| `dataset/` | Bundle, readers, merge, index seek |
| `book/` | Shared file book, deltas, tick math |
| `impact/` | eaten + recovery |
| `match/` | MatchOne vs effective view |
| `population/` | slots, pop_ts, TapeWindow, Advance |
| `transport/grpc/` | OpenPopulation + bidi stream |
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
конфиг/слоты не меняем (§5.2).

**Идентификатор ордера:** отдельного venue-`order_id` **нет**. Ключ =
`(slot_id, client_order_id)`. Клиент задаёт `client_order_id` на `Order`; все
ack / partial / fill / cancel / reject — по нему. `Cancel` несёт **тот же**
`client_order_id`, что был на `Order`. После терминального состояния id мёртв;
новый ордер — только с новым id. Уникальность в пределах слота — обязанность
PS; venue reject’ит дубликат живого id.

**Ещё открыто:** точные имена RPC; attach-dataset отдельно или внутри Open.

### 8.3 Кодирование book на wire — **закрыто**

- Bundle в Open: 1…N `Component` с уникальным `component_id`; `role` = класс.
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

- Несколько stream’ов в одном процессе venue: thread-per-population? пул?
- CPU quantum между популяциями.

### 8.9 Ускорение (не P0)

- `tape_stride` в Open уже есть: P0 держать `1`; `N > 1` — склейка N событий
  файла в один Tick (обычно last book), один round-trip на склейку.
- Бинарный датасет вместо JSONL на горячем пути.

### 8.10 IDL и совместимость — **закрыто (каркас)**

Один bidi stream; первое сообщение клиента = Open; цикл Tick → Orders →
Advance; нормальный конец = venue `End`; опционально клиент `Close` =
graceful shutdown only. In-process API не в P0.

Точные имена proto/package/версия — при первом `.proto`, без смены семантики.

### 8.11 Документы

- Позже синхронизировать [`POPULATION_PLAN.md`](POPULATION_PLAN.md) /
  [`GLOSSARY.md`](GLOSSARY.md) с этим ТЗ (по отдельному решению).
- ADR на «один `pop_ts` + pull gRPC + model D» — по запросу.

---

## 9. Зафиксированные default’ы (шпаргалка)

| Вопрос | Default |
| --- | --- |
| Конкуренция слотов за книгу | Нет; свой effective + impact |
| Часы популяции | Один `pop_ts` |
| Видимость | Model D (`md_lag` + `Δ`); B = `md_lag=0` |
| Лента | SnapshotFloor + BookDelta window |
| Кто двигает время | Population Service (pull Advance) |
| Шаг | Каждый тик, со стримом динамики |
| Транспорт | gRPC 1 stream: Open→цикл; End от venue; Close от клиента = graceful only; in-process нет |
| `tape_stride` | Склейка N событий файла в один Tick; P0 = 1; не путать с price `tick_size` |
| Старт ленты | Всегда первое событие файла; mid-file seek нет |
| View на wire | books/overlays keyed by `component_id` + trades |
| Bundle | `component_id` уникален; `role` = SPOT/FUTURE/OPTION; P0 ≥ 1 |
| Выравнивание лент | Merge по `ts_ns` + keep last; без оффлайн-решётки |
| Open / mid-run | `population_id`+params → fix; конфиг mid-run не трогаем |
| Лимит L2 | 64 уровня на сторону |
| Id ордера | `(slot_id, client_order_id)`; venue-`order_id` нет; Cancel = тот же id |
| Resting priority в слоте | Порядок отправки стратегией (раньше → раньше в матче) |
| Между стратегиями | Нет конкуренции, нет видимости чужих филлов; Tick — батч по слотам |
| TIF | DAY / IOC / FOK; `UNSPECIFIED` → `Error` |
| Side | BUY / SELL; `UNSPECIFIED` → `Error` |
| Phase 1 host | Отдельный Population Service (не embed-only) |
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
