# План реализации: population + UDS/SPSC

Канон семантики: [`POPULATION_TZ.md`](POPULATION_TZ.md).  
Этот документ — **порядок работ**, чтобы уже писать код. Не дублирует ТЗ.

**Не трогаем в этих вехах:** CMF `make run` / `src/venue/` (smoke остаётся).  
**Позже:** квоты, recoverable `OrdersAck`, index, multi-population fairness.

---

## Что уже есть (якорь)

| Кусок | Статус |
| --- | --- |
| `src/population/*` | 1 component: Start / MakeTick / Submit / Advance, impact, match, tests |
| `src/dataset/jsonl_tape.*` | Tape ← JSONL |
| `protos/messages.proto` | Схема сообщений (без gRPC runtime) |
| `third_party/ipc.h` | SPMC ring (не SPSC); кандидат на out/in или эталон |
| gRPC / `rpc.proto` service | **не делаем**; service можно игнорить |

Gap до ТЗ: нет UDS+очередей+SessionEnded; нет 1…3 книг; Submit ещё думает про recoverable Ack.

---

## Целевой каркас модулей

```text
apps/population_venue/     # или расширить backtest: listen UDS
src/transport/uds/         # create | reject | SessionEnded
src/transport/spsc/        # пара колец на прогон (+ обёртка над ipc/своим SPSC)
src/transport/session/     # state machine §5.3–5.5 + drain Close|timeout
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
| 1.1 | UDS server: listen, accept, `CreateQueues` → две очереди \| reject | тест: клиент получает handles |
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
| 2.1 | Map wire ↔ in-memory: Open / TickView / OrderBatch / Advance / Close / End / Error (protobuf serialize **или** простой binary; без gRPC) | round-trip тест на Tick + Order |
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
| 3.1 | `MergedTape` / scheduler: N `Tape` → события, упорядоченные по `ts_ns`; на шаге keep-last per component | unit: 2 ленты с дырами |
| 3.2 | `Population` → multi-component: FileBook / TapeWindow / Impact / Working **per component_id** | API Open.components 1…3 |
| 3.3 | MakeTick: SlotTick.books[] по всем ногам, у которых уже было first event; нет ноги → нет book | тест spot+fut; тест только fut |
| 3.4 | Match/Submit с `component_id`; неизвестный id → fatal (пока) | тест |
| 3.5 | Один `pop_ts`, один Advance двигает merge | детерминизм: два прогона → те же trades |

Рефактор: не плодить три `Population`; одна популяция, map `component_id → leg state`.

---

## Веха 4 — Полировка, долг, стыковка с доками

| # | Задача | Done when |
| --- | --- | --- |
| 4.1 | Один `PriceToTicks` util (см. `TODO_PRICE_TO_TICKS.md`) | нет локальных копий в population path |
| 4.2 | Решить §8.4 impact@view_ts (упрощение ok для P0: current eaten − old file snap) | ADR-строка в ТЗ или комментарий + тест |
| 4.3 | README: UDS path, как поднять population venue + fake client | абзац + make target |
| 4.4 | `protos/rpc.proto`: пометка deprecated / не генерить gRPC | generate.sh не требует plugin |

---

## Явно не сейчас

- Квоты на Create  
- Recoverable `OrdersAck` (retry batch)  
- Mid-file index / seek  
- Несколько популяций + fairness  
- Калибровка `λ`  
- Замена JSONL на бинарь  

---

## Рекомендуемый порядок первых PR

1. **SPSC/ring smoke** (0.2) + structs UDS (0.1)  
2. **Session lifecycle** (1.1–1.5) со stub population  
3. **Wire + real Population 1-book** (2.x)  
4. **MergedTape + 1…3 books** (3.x)  
5. Полировка (4.x)

Каждый PR: тесты на Done when; CMF `make test` / `make run` зелёные если задеты.

---

## Критерий «можно писать прямо сейчас»

Стартовая точка без открытых дизайн-развилок:

- Писать **кольцо + Create/SessionEnded** (веха 0–1).  
- Population ядро **не блокирует** — подключается в вехе 2.  
- Bundle — отдельная веха после зелёного single-component на UDS/SPSC.

Открытые мелочи (fd vs path, reserved slot при полной out) — выбрать минимально в 0.2/1.3 и зафиксировать комментарием; не ждать идеального IPC.
