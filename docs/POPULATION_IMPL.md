# План реализации: population + UDS/SPSC

Канон семантики: [`POPULATION_TZ.md`](POPULATION_TZ.md).  
Ориентация / layout для агентов: [`CONTEXT.md`](CONTEXT.md).  
Этот документ — **порядок работ**, чтобы уже писать код. Не дублирует ТЗ.

**CMF TCP path снят** (`backtest` / `cmf_driver` / `src/venue` удалены); работаем только на population UDS.  
**Позже:** квоты, recoverable `OrdersAck`, index, multi-population fairness.

---

## Что уже есть (якорь)

| Кусок | Статус |
| --- | --- |
| `src/population/*` | 1…3 components via MergedTape: Start / MakeTick / Submit / Advance, impact, match, tests |
| `src/dataset/jsonl_tape.*` | Tape ← JSONL |
| `src/transport/*` | UDS control + **POSIX shm SPSC** + bus + session (binary framing) |
| `apps/population_venue` + smoke + client | listen UDS; smoke/client attach shm by CreateAck names |
| `third_party/ipc.h` | не подключён (эталон SPMC; свой SPSC уже есть) |

Gap до ТЗ: Submit ещё без recoverable Ack; квоты / index / multi-pop fairness / fd-passing — позже.

---

## Целевой каркас модулей

```text
apps/population_venue/     # UDS listen → App::Run
src/population_venue/      # config + application
src/transport/             # UDS create/SessionEnded + SPSC + session SM
src/population/            # ядро (расширить до bundle)
src/dataset/               # + merge timeline 1…3 JSONL
```

Глубокий шов: **`Population` / будущий `BundlePopulation`** — тик/ордера/advance.  
Транспорт снаружи только сериализует и гоняет state machine сессии.

---

## Веха 0 — Заготовки, которые разблокируют остальное

**Зачем:** один раз решить фрейминг и типы сессии, чтобы M1–M3 не переписывать.

| # | Задача | Done when |
| --- | --- | --- |
| 0.1 | Зафиксировать control-сообщения UDS: `CreateQueues`, `CreateAck` (handles \| reject), `SessionEnded{ok\|fatal, reason_code, message}` | struct’ы / enum в `src/transport/uds/` (+ короткий комментарий = ТЗ §5.5) |
| 0.2 | Выбрать IPC: обернуть `third_party/ipc.h` (1 consumer ≈ SPSC) **или** тонкий свой SPSC; одна пара out/in на прогон | header + smoke unit (push/pop) |
| 0.3 | Договориться handles: shm name / path (пока строка в `CreateAck`; fd-passing — позже если нужно) | записано в коде CreateAck |
| 0.4 | `drain_timeout_ms` (default 60000) в конфиге сессии | поле читается тестом |

**Первый коммит можно начать с 0.2** (кольцо) — не зависит от population.

---

## Веха 1 — Session без книг (transport + lifecycle)

**Зачем:** ошибки и Close/timeout работают **до** сложной population.

| # | Задача | Done when |
| --- | --- | --- |
| 1.1 | UDS server: **один** listen/accept от Engine; `Create` → 2×SPSC \| reject; UDS не рвать после SessionEnded | тест: два Create подряд на одном fd |
| 1.2 | `Session` driver: фазы AwaitOpen → … (пока **stub** population: echo/фикс Tick) | state machine покрыта unit’ами |
| 1.3 | Fatal path: Error в out → `SessionEnded{fatal, code, short msg}` → ждать **Close** \| timeout → teardown | тест: Close приходит → очереди сняты; тест: нет Close → timeout teardown + message не пустой |
| 1.4 | Normal path: End → `SessionEnded{ok}` → Close \| timeout | симметричный тест |
| 1.5 | Create reject (без очередей): только UDS | тест без SPSC |

Stub population: один фейковый Tick / сразу End — достаточно проверить транспорт.

---

## Веха 2 — Подключить существующую 1-книжную Population

**Зачем:** первый полезный прогон Engine ↔ venue по новому транспорту.

| # | Задача | Done when |
| --- | --- | --- |
| 2.1 | Map wire ↔ in-memory: Open / TickView / OrderBatch / Advance / Close / End / Error (свой binary framing) | round-trip тест на Tick + Order |
| 2.2 | Session вызывает `Population::{Start,MakeTick,Submit,Advance}` | один JSONL, 1–N slots, fake engine в тесте гоняет цикл §5.4 |
| 2.3 | Битый Open / protocol violation → fatal §5.5 (не recoverable) | тест |
| 2.4 | Битый OrderBatch → **пока fatal** (recoverable Ack — позже) | тест |
| 2.5 | App entry: `make population` / бинарь слушает UDS path из env/toml | ручной smoke с минимальным клиентом (C++ или Python) |

CMF binary не ломаем. Population lib линкуем в новый app.

---

## Веха 3 — Bundle: до 3 книг (spot / futures / option)

**Зачем:** канон ТЗ; 1 книга = частный случай.

| # | Задача | Done when |
| --- | --- | --- |
| 3.1 | `MergedTape` / scheduler: N `Tape` → события, упорядоченные по `ts_ns`; на шаге keep-last per component | ✅ unit: 2 ленты с дырами (`merged_tape_test`) |
| 3.2 | `Population` → multi-component: FileBook / TapeWindow / Impact / Working **per component_id** | ✅ API Open.components 1…3 (`population_bundle_test`) |
| 3.3 | MakeTick: SlotTick.books[] по всем ногам, у которых уже было first event; нет ноги → нет book | ✅ `MakeTickSpotAndFutKeyframes` / `MakeTickOnlyFutUntilSpotStarts` |
| 3.4 | Match/Submit с `component_id`; неизвестный id → fatal (пока) | ✅ `SubmitRejectsUnknownAndNotPresentComponent` / `MatchUsesOwnComponentBook` |
| 3.5 | Один `pop_ts`, один Advance двигает merge | ✅ `TwoRunsSameTrades` |

Рефактор: не плодить три `Population`; одна популяция, map `component_id → component state`.

---

## Веха 4 — Полировка, долг, стыковка с доками

| # | Задача | Done when |
| --- | --- | --- |
| 4.1 | Один `PriceToTicks` util (см. `TODO_PRICE_TO_TICKS.md`) | ✅ `population/price_ticks.hpp` |
| 4.2 | Решить §8.4 impact@view_ts (упрощение ok для P0: current eaten − old file snap) | ✅ ADR в ТЗ §8.4; `MdLagAppliesCurrentImpactToOldFileSnap` |
| 4.3 | README: UDS path, как поднять population venue + fake client | ✅ README «Run venue + fake client» + `make population-smoke` |

---

## Веха 5 — Ops: graceful shutdown + логер (в самом конце)

**Зачем:** venue как долгоживущий сервис: Ctrl+C не рвёт сессии на полуслове; логи читаемы людьми и машиной.

| # | Задача | Done when |
| --- | --- | --- |
| 5.1 | Graceful shutdown по **Ctrl+C** (`SIGINT` / при необходимости `SIGTERM`): перестать Accept/Create, дождаться drain текущих `PopulationSession` (или их `Finish*` с понятным reason), затем teardown listener/UDS | ✅ SIGINT/SIGTERM → stop accept/create; sessions `FinishFatal("venue shutdown")`; exit 0 |
| 5.2 | Логер: **уровни** (trace/debug/info/warn/error), два формата вывода — **plain** (человекочитаемый) и **JSON** (одна строка / event); level/format **compile-time** (`CMF_LOG_*`) | ✅ `LOG_*` + `CMF_LOG_LEVEL`/`CMF_LOG_FORMAT` |
| 5.3 | Не писать логер с нуля, если можно взять готовое: **в этом репо логера нет**; кандидаты снаружи — `spdlog` (levels + sinks; JSON через pattern/custom sink) или тонкая обёртка над ним. Выбрать одно, зафиксировать в CMake/FetchContent | ✅ spdlog v1.15.1 FetchContent; README usage |

Порядок внутри вехи: сначала 5.1 (не зависит от логера), затем 5.2–5.3 (можно заменить `fprintf` по мере появления sinks).

---

## Явно не сейчас

- Квоты на Create  
- Recoverable `OrdersAck` (retry batch)  
- Mid-file index / seek  
- Несколько популяций + fairness  
- Калибровка `λ`  
- Замена JSONL на бинарь  
- fd-passing handles вместо shm names  

---

## Рекомендуемый порядок первых PR

1. **SPSC/ring smoke** (0.2) + structs UDS (0.1)  
2. **Session lifecycle** (1.1–1.5) со stub population  
3. **Wire + real Population 1-book** (2.x)  
4. **MergedTape + 1…3 books** (3.x)  
5. Полировка (4.x)  
6. **Graceful Ctrl+C + логер plain/JSON** (5.x) — после рабочего venue

Каждый PR: тесты на Done when; `make test` / `make population-smoke` зелёные.

---

## Критерий «можно писать прямо сейчас»

Стартовая точка без открытых дизайн-развилок:

- Писать **кольцо + Create/SessionEnded** (веха 0–1).  
- Population ядро **не блокирует** — подключается в вехе 2.  
- Bundle — отдельная веха после зелёного single-component на UDS/SPSC.

Открытые мелочи (fd vs path, reserved slot при полной out) — выбрать минимально в 0.2/1.3 и зафиксировать комментарием; не ждать идеального IPC.
