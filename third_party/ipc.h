#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

/// Точка инъекции для тестов: вклинивается между проверкой штампа «до»
/// и копированием payload — ровно в то окно, где живёт гонка рваного
/// чтения. В обычной сборке разворачивается в ничто.
#ifndef SPMC_TEST_HOOK_BEFORE_COPY
#define SPMC_TEST_HOOK_BEFORE_COPY() ((void)0)
#endif

/// Своя константа, а не std::hardware_destructive_interference_size: та
/// не ABI-стабильна, её значение зависит от -mtune, и два независимо
/// собранных бинаря получили бы структуры разного размера.
constexpr std::uint64_t cache_line_size = 64;

constexpr std::uint64_t invalid_magic_number    = 0;
constexpr std::uint32_t invalid_major_version   = 0;
constexpr std::uint32_t invalid_minor_version   = 0;
constexpr std::uint64_t invalid_ring_size_pow_2 = 0;

/// ─────────────────────────────────────────────────────────────────────────
/// ТИП СООБЩЕНИЯ. Заменяется под проект — это единственное место.
///
/// Требование одно: POD. Тело копируется memcpy и валидируется штампом,
/// поэтому нетривиальный конструктор, деструктор или виртуальный метод
/// сломали бы всю схему. Указателей внутри быть не должно по другой
/// причине: процессы мапят сегмент по разным адресам.
/// ─────────────────────────────────────────────────────────────────────────
struct spmc_payload_t {
    std::uint64_t words[7];
};

struct data_slot_t {
    /// Единственное слово состояния слота:
    ///     бит 0      — in_progress, писатель внутри
    ///     бит 1      — empty, пустая запись от отката
    ///     биты 2..63 — номер сообщения
    ///
    /// Размер тела тут НЕ хранится намеренно. Он фиксирован и известен на
    /// компиляции, поэтому читателю не приходится брать длину memcpy из
    /// памяти, которая в этот момент может быть рваной.
    std::uint64_t contained_message_number{0};

    spmc_payload_t payload;
};

/// Сколько байт копируется. Константа компиляции, а не значение из слота —
/// отсюда и безопасность: выйти за границы буфера нечем.
inline constexpr std::uint64_t payload_bytes = sizeof(spmc_payload_t);

/// Биты состояния в штампе
inline constexpr std::uint64_t stamp_in_progress = 1u;
inline constexpr std::uint64_t stamp_empty       = 2u;
inline constexpr std::uint64_t stamp_seq_shift   = 2u;

/// Header of the flat struct used for sending events to strategies
/// SPMC queue designed for low-latency applications
struct spmc_ring_buffer_t {
    static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);
    static constexpr std::uint64_t magic_number_value = 0x00AB0CAD0;

    /// ── Холодная часть ───────────────────────────────────────────────
    /// Пишется один раз при создании. Читается всеми: ring_size_pow_2 нужен
    /// для маски, offset и stride — для адресации слота, то есть на КАЖДОМ
    /// try_read. Эти поля обязаны лежать в строке, которую никто не пишет.
    std::uint64_t magic_number{invalid_magic_number};
    std::uint32_t major_version{invalid_major_version};
    std::uint32_t minor_version{invalid_minor_version};

    /// Contains actual ring size
    std::uint64_t ring_size_pow_2{invalid_ring_size_pow_2};

    /// Slots array info
    std::uint64_t data_slots_array_offset{0};
    std::uint64_t single_slot_stride{0};

    /// Меняется при КАЖДОМ конструировании кольца. Без этого поля
    /// перезапуск писателя на том же сегменте означает, что подписчики
    /// зависают навсегда: их expected выше любого номера в новом кольце,
    /// и состояние неотличимо от тишины на фиде.
    std::uint64_t session_id{0};

    /// Хэш раскладки, посчитанный из sizeof и offsetof. Ловит физическое
    /// расхождение ABI между независимо собранными бинарями: другой -mtune,
    /// другая версия компилятора, добавленное поле. Версии такое не видят,
    /// а бампать их вручную всё равно забывают.
    std::uint64_t layout_hash{0};

    /// ── Горячая часть ────────────────────────────────────────────────
    /// Писатель дёргает это поле на каждом батче. ПОСЛЕДНИМ в структуре:
    /// alignas выравнивает только начало поля и не отгораживает его снизу,
    /// поэтому любое поле после него попало бы в ту же строку и ловило бы
    /// инвалидацию на каждую публикацию.
    alignas(cache_line_size) std::uint64_t global_message_number{0};
};

/// Раскладка — это ABI между двумя независимо собираемыми бинарями.
/// Пусть ломается на компиляции, а не в рантайме.
static_assert(sizeof(spmc_ring_buffer_t) == 2 * cache_line_size);
static_assert(offsetof(spmc_ring_buffer_t, magic_number) == 0);
static_assert(offsetof(spmc_ring_buffer_t, global_message_number) == cache_line_size);
static_assert(std::is_trivially_copyable_v<spmc_ring_buffer_t>);
/// FNV-1a по 8 байтам. Считается на компиляции, в рантайме это константа.
constexpr std::uint64_t spmc_layout_mix(std::uint64_t h, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        h = (h ^ ((v >> (i * 8)) & 0xFFu)) * 0x100000001B3ull;
    }
    return h;
}

/// Хэш физической раскладки сегмента. Любое изменение размера или смещения
/// любого поля меняет это число, поэтому рассинхронизироваться с раскладкой
/// оно не может — в отличие от major_version, которую надо бампать руками.
inline constexpr std::uint64_t spmc_layout_hash_value = [] {
    std::uint64_t h = 0xCBF29CE484222325ull;
    h = spmc_layout_mix(h, sizeof(spmc_ring_buffer_t));
    h = spmc_layout_mix(h, offsetof(spmc_ring_buffer_t, magic_number));
    h = spmc_layout_mix(h, offsetof(spmc_ring_buffer_t, major_version));
    h = spmc_layout_mix(h, offsetof(spmc_ring_buffer_t, minor_version));
    h = spmc_layout_mix(h, offsetof(spmc_ring_buffer_t, ring_size_pow_2));
    h = spmc_layout_mix(h, offsetof(spmc_ring_buffer_t, data_slots_array_offset));
    h = spmc_layout_mix(h, offsetof(spmc_ring_buffer_t, single_slot_stride));
    h = spmc_layout_mix(h, offsetof(spmc_ring_buffer_t, session_id));
    h = spmc_layout_mix(h, offsetof(spmc_ring_buffer_t, layout_hash));
    h = spmc_layout_mix(h, offsetof(spmc_ring_buffer_t, global_message_number));
    h = spmc_layout_mix(h, sizeof(data_slot_t));
    h = spmc_layout_mix(h, offsetof(data_slot_t, contained_message_number));
    h = spmc_layout_mix(h, offsetof(data_slot_t, payload));
    h = spmc_layout_mix(h, payload_bytes);
    h = spmc_layout_mix(h, cache_line_size);
    return h;
}();

static_assert(std::is_trivially_copyable_v<spmc_payload_t>,
              "тип сообщения обязан быть POD: тело копируется memcpy "
              "и живёт в разделяемой памяти");
static_assert(std::is_trivially_default_constructible_v<spmc_payload_t>);
static_assert(std::is_trivially_copyable_v<data_slot_t>);
static_assert(offsetof(data_slot_t, contained_message_number) == 0);
static_assert(alignof(data_slot_t) <= cache_line_size,
              "тип сообщения требует выравнивания больше строки кэша");
static_assert(payload_bytes > 0);

/// Тип сообщения обязан заполнять слот РОВНО, без хвоста между sizeof
/// и stride. Иначе писатель через payload_capacity мог бы записать больше,
/// чем читатель получает в теле слота, — асимметрия, которая проявилась бы
/// как out_buffer_too_small у корректного вызывающего.
///
/// Заменил тип и получил эту ошибку — добей его паддингом до кратности
/// строке кэша минус sizeof заголовка слота.
static_assert(sizeof(data_slot_t) % cache_line_size == 0,
              "тип сообщения должен заполнять слот ровно: добей паддингом "
              "до кратности cache_line_size");
static_assert(payload_bytes == sizeof(data_slot_t) - offsetof(data_slot_t, payload));

enum spmc_ring_error_t {
    no_error = 0,

    // Contruction error 10 - 99
    shmem_buf_is_null = 10,
    shmem_buf_is_not_large_enough = 11,
    ring_size_hint_is_zero = 12,
    ring_size_hint_causes_overflow = 13,
    shmem_buf_is_misaligned = 14,


    // Writer errors 100 - 199
    writer_state_is_null = 100,
    nested_claim_not_allowed = 101,
    claim_was_not_opened = 102,

    // Reader errors 200 - 299
    reader_state_is_null = 200,
    ring_is_not_ready = 201,
    unsupported_major_version = 202,
    message_not_published_yet = 203,   /// слот держит прошлый круг — ждём
    writer_is_inside_slot = 204,       /// нечётный номер — писатель внутри, ждём
    message_was_lapped = 205,          /// обогнали, сообщение мертво — см. out_lost
    empty_record = 206,                /// откат писателя, сообщения нет — идём дальше
    out_buffer_too_small = 207,
    layout_mismatch = 208,             /// бинари собраны с разной раскладкой
    session_changed = 209,             /// писатель пересоздал кольцо, нужен re-attach
};


/// Куда прыгать после обгона.
enum spmc_ring_resync_policy_t {
    /// В голову. Теряем больше за раз, зато получаем целое кольцо запаса
    /// до следующего обгона. Для раздачи ивентов — обычно правильный выбор.
    resync_to_newest = 0,

    /// В начало живого окна. Забираем максимум уцелевшего, но стартуем
    /// в одном слоте от затирания: если писатель быстрее, обгон повторится
    /// немедленно и получится спираль.
    resync_to_oldest_alive = 1,
};

/// Used by producer
[[nodiscard]] inline spmc_ring_error_t
construct_at(void* shmem_buf, std::uint64_t shmem_buf_size_bytes,
             [[maybe_unused]] std::uint64_t ring_size_hint,
             [[maybe_unused]] std::uint64_t expected_readers_hint,
             spmc_ring_buffer_t*& out) noexcept
{
    if (shmem_buf == NULL) [[unlikely]] {
        out = NULL;
        return spmc_ring_error_t::shmem_buf_is_null;
    }

    if (ring_size_hint == 0) [[unlikely]] {
        out = NULL;
        return spmc_ring_error_t::ring_size_hint_is_zero;
    }

    /// Вся раскладка строится от этого адреса. Если он не выровнен на строку
    /// кэша, misaligned окажется КАЖДЫЙ слот, и фолш-шаринг вернётся целиком.
    /// mmap отдаёт страницу и условие выполняет всегда, но полагаться на это
    /// молча нельзя — тесты и кастомные аллокаторы дают что угодно.
    if ((reinterpret_cast<std::uintptr_t>(shmem_buf) % cache_line_size) != 0) [[unlikely]] {
        out = NULL;
        return spmc_ring_error_t::shmem_buf_is_misaligned;
    }

    /// TODO use start_lifetime_as instead {
    void* tmp = std::memmove(shmem_buf, shmem_buf, sizeof(spmc_ring_buffer_t));
    out = std::launder(static_cast<spmc_ring_buffer_t*>(tmp));
    // }

    out->ring_size_pow_2 = std::bit_ceil(ring_size_hint);

    /// Stride — это шаг между началами слотов, а не размер содержимого.
    /// Округляем вверх до строки кэша, чтобы соседние слоты её не делили.
    out->single_slot_stride = (sizeof(data_slot_t) + cache_line_size - 1)
                            & ~(cache_line_size - 1);

    /// Слоты начинаются после хедера, тоже с выравниванием на строку —
    /// иначе смещение поедет сразу у всех слотов.
    out->data_slots_array_offset = (sizeof(spmc_ring_buffer_t) + cache_line_size - 1)
                                 & ~(cache_line_size - 1);

    bool overflow = out->ring_size_pow_2 > (UINT64_MAX - out->data_slots_array_offset) / out->single_slot_stride;
    if (overflow) [[unlikely]] {
        out = NULL;
        return spmc_ring_error_t::ring_size_hint_causes_overflow;
    }

    std::uint64_t total_structure_size_bytes =
        out->ring_size_pow_2 * out->single_slot_stride + out->data_slots_array_offset;

    if (total_structure_size_bytes > shmem_buf_size_bytes) [[unlikely]] {
        return spmc_ring_error_t::shmem_buf_is_not_large_enough;
    }

    /// ── Инвалидация слотов ───────────────────────────────────────────
    /// Обязательна, и вот почему. На свежем сегменте ftruncate уже дал нули,
    /// но при пересоздании кольца на ТОМ ЖЕ сегменте в слотах остаются
    /// штампы прошлого писателя — с номерами намного выше нового нуля.
    ///
    /// Без этой очистки переподключившийся читатель находит штамп от
    /// прошлой сессии, он больше ожидаемого, срабатывает ресинк, и в
    /// зависимости от политики читатель либо теряет всё опубликованное
    /// новым писателем, либо — что хуже — прыгает на старый номер и
    /// ОТДАЁТ НАВЕРХ ДАННЫЕ МЁРТВОЙ СЕССИИ как актуальные. session_id
    /// это не ловит: он уже совпал при переподключении.
    ///
    /// Цена — один проход с записью 8 байт на слот, только при создании.
    /// Тело слота не трогаем: без совпадающего штампа оно недостижимо.
    {
        std::byte* slots = reinterpret_cast<std::byte*>(out) + out->data_slots_array_offset;
        for (std::uint64_t i = 0; i < out->ring_size_pow_2; ++i) {
            std::byte* slot = slots + i * out->single_slot_stride;
            std::atomic_ref<std::uint64_t>{
                reinterpret_cast<data_slot_t*>(slot)->contained_message_number}
                .store(0, std::memory_order_relaxed);
        }
    }

    out->layout_hash = spmc_layout_hash_value;

    /// Инкремент, а не константа: на свежем сегменте здесь ноль и получится 1,
    /// на переиспользуемом — следующее значение. Подписчик сравнивает со своей
    /// копией, снятой при подключении, поэтому любой сдвиг он заметит.
    ///
    /// Читаем до того, как выставим magic: пока барьер готовности не поднят,
    /// старые подписчики всё равно не вправе ничего интерпретировать.
    out->session_id = out->session_id + 1;

    /// Следующий к публикации. Единица, а не ноль: ноль зарезервирован под
    /// «слот пуст», и читатель, стартовавший с нуля, принял бы нетронутый
    /// слот за валидное нулевое сообщение.
    out->global_message_number = 1;

    std::atomic_ref<std::uint64_t> magic_ref{out->magic_number};
    magic_ref.store(spmc_ring_buffer_t::magic_number_value, std::memory_order_release);

    return spmc_ring_error_t::no_error;
}


/// Used by consumer
[[nodiscard]] inline spmc_ring_error_t
assume_constructed_at(void* shmem_buf, [[maybe_unused]] std::uint64_t shmem_buf_size_bytes, spmc_ring_buffer_t*& out) noexcept {
    if (shmem_buf == NULL) [[unlikely]] {
        out = NULL;
        return spmc_ring_error_t::shmem_buf_is_null;
    }

    out = std::launder(static_cast<spmc_ring_buffer_t*>(shmem_buf));

    return spmc_ring_error_t::no_error;
}


/// Адрес слота, в котором живёт сообщение с данным номером.
///
/// Шаг берётся из хедера, а не из sizeof — иначе размер сообщения был бы
/// зашит в бинарь на этапе компиляции.
///
/// Маска применяется ВНУТРИ: функция тотальна, выйти за пределы кольца
/// невозможно по построению. AND бесплатен, а частичная функция, которая
/// полагается на дисциплину вызывающего, — лишний источник ошибок.
[[nodiscard]] inline data_slot_t*
spmc_ring_slot_at(spmc_ring_buffer_t* self, std::uint64_t message_number) noexcept {
    const std::uint64_t idx = message_number & (self->ring_size_pow_2 - 1);
    std::byte* base = reinterpret_cast<std::byte*>(self) + self->data_slots_array_offset;
    return reinterpret_cast<data_slot_t*>(base + idx * self->single_slot_stride);
}


/// Состояние писателя. Живёт в приватной памяти процесса, НЕ в shmem:
/// счётчик дёргается на каждом сообщении и не должен стоить когерентности.
struct spmc_ring_writer_local_state_t {
    /// Следующий номер к публикации. Стартует с 1 — ноль занят под «слот пуст».
    std::uint64_t next_message_number{1};

    /// Заполняется в claim, потребляется в commit/abort.
    /// NULL означает, что открытого claim нет.
    data_slot_t*  claimed_slot{NULL};
    std::uint64_t claimed_message_number{0};
    void*         payload_buffer{NULL};
    std::uint64_t payload_capacity{0};
};


/// Захватывает слот под следующее сообщение и отдаёт указатель, по которому
/// можно сериализовать напрямую в shmem, без промежуточной копии.
///
/// Пока claim открыт, слот принадлежит писателю эксклюзивно: нечётный номер
/// отсекает всех читателей. Поэтому запись на месте здесь безопасна —
/// в отличие от чтения, где копия обязательна.
///
/// Участок между claim и commit обязан быть коротким и noexcept. Если
/// управление уйдёт отсюда, не закрыв claim, слот навсегда останется
/// нечётным, и каждый дошедший до него читатель встанет на нём навечно.
[[nodiscard]] inline spmc_ring_error_t
spmc_ring_claim(spmc_ring_buffer_t* self, spmc_ring_writer_local_state_t* state) noexcept {
    if (self == NULL || state == NULL) [[unlikely]] {
        return spmc_ring_error_t::writer_state_is_null;
    }

    if (state->claimed_slot != NULL) [[unlikely]] {
        return spmc_ring_error_t::nested_claim_not_allowed;
    }

    const std::uint64_t seq = state->next_message_number;
    const std::uint64_t idx = seq & (self->ring_size_pow_2 - 1);

    data_slot_t* slot = spmc_ring_slot_at(self, idx);

    /// Слот объявлен невалидным ДО того, как тронут payload.
    std::atomic_ref<std::uint64_t>{slot->contained_message_number}
        .store((seq << stamp_seq_shift) | stamp_in_progress, std::memory_order_relaxed);

    /// StoreStore. Нечётный номер обязан стать видимым раньше байт payload.
    /// store_release здесь не подошёл бы: он упорядочивает то, что ДО него,
    /// а нам нужно упорядочить то, что ПОСЛЕ. На x86 разворачивается в ноль
    /// инструкций, на ARM без него — редкий невоспроизводимый корапт.
    std::atomic_thread_fence(std::memory_order_release);

    state->claimed_slot           = slot;
    state->claimed_message_number = seq;
    state->payload_buffer         = &slot->payload;
    state->payload_capacity       = self->single_slot_stride - offsetof(data_slot_t, payload);

    return spmc_ring_error_t::no_error;
}


/// Публикует захваченный слот. Размер фиксирован, поэтому параметра нет
/// и нет пути ошибки «не влезло».
[[nodiscard]] inline spmc_ring_error_t
spmc_ring_commit(spmc_ring_buffer_t* self, spmc_ring_writer_local_state_t* state,
                 std::uint64_t flags = 0) noexcept {
    if (self == NULL || state == NULL) [[unlikely]] {
        return spmc_ring_error_t::writer_state_is_null;
    }

    if (state->claimed_slot == NULL) [[unlikely]] {
        return spmc_ring_error_t::claim_was_not_opened;
    }

    const std::uint64_t seq = state->claimed_message_number;

    /// Release: все записи payload обязаны стать видимыми раньше штампа
    /// с снятым битом in_progress.
    std::atomic_ref<std::uint64_t>{state->claimed_slot->contained_message_number}
        .store((seq << stamp_seq_shift) | (flags & stamp_empty),
               std::memory_order_release);

    state->next_message_number = seq + 1;
    state->claimed_slot        = NULL;

    return spmc_ring_error_t::no_error;
}


/// Откат, когда сериализация не удалась. Байты payload могли быть частично
/// испорчены, поэтому вернуть слот прежнему владельцу нельзя: читатель,
/// ждущий старое сообщение, совпал бы по номеру и принял мусор за валидные
/// данные — обнаруженная потеря превратилась бы в необнаруженный корапт.
///
/// Публикуем пустую запись: бит empty в штампе. Номер сообщения расходуется,
/// дырок в нумерации нет.
[[nodiscard]] inline spmc_ring_error_t
spmc_ring_abort(spmc_ring_buffer_t* self, spmc_ring_writer_local_state_t* state) noexcept {
    return spmc_ring_commit(self, state, stamp_empty);
}


/// Откат, когда payload гарантированно не тронут — например, валидация
/// отвергла сообщение до вызова сериализатора. Слот возвращается прежнему
/// владельцу, номер сообщения НЕ расходуется.
///
/// Предыдущего владельца не нужно читать, он вычисляется: seq - ring_size.
///
/// Пользуйся только там, где по коду видно, что в payload_buffer не писали.
/// Во всех остальных случаях spmc_ring_abort — он всегда корректен и не
/// требует рассуждений.
[[nodiscard]] inline spmc_ring_error_t
spmc_ring_abort_untouched(spmc_ring_buffer_t* self,
                          spmc_ring_writer_local_state_t* state) noexcept {
    if (self == NULL || state == NULL) [[unlikely]] {
        return spmc_ring_error_t::writer_state_is_null;
    }

    if (state->claimed_slot == NULL) [[unlikely]] {
        return spmc_ring_error_t::claim_was_not_opened;
    }

    const std::uint64_t seq       = state->claimed_message_number;
    const std::uint64_t ring_size = self->ring_size_pow_2;

    /// Слот на первом круге ещё никем не занимался — возвращаем «пусто».
    const std::uint64_t previous_owner = (seq > ring_size)
                                       ? ((seq - ring_size) << stamp_seq_shift)
                                       : 0;

    std::atomic_ref<std::uint64_t>{state->claimed_slot->contained_message_number}
        .store(previous_owner, std::memory_order_release);

    /// next_message_number не трогаем — seq переиспользуется следующим claim
    state->claimed_slot = NULL;

    return spmc_ring_error_t::no_error;
}


/// Выкладывает счётчик писателя в хедер. Вызывать раз в батч, а не на каждое
/// сообщение: строка с global_message_number разделяется с читателями, и
/// каждая запись в неё инвалидирует их копии.
///
/// В горячем пути чтения этот счётчик не нужен — читателю хватает номера
/// в самом слоте. Он нужен только для подключения новых читателей и для
/// ресинка после обгона.
inline void
spmc_ring_publish_cursor(spmc_ring_buffer_t* self,
                         const spmc_ring_writer_local_state_t* state) noexcept {
    std::atomic_ref<std::uint64_t>{self->global_message_number}
        .store(state->next_message_number, std::memory_order_release);
}







/// ─────────────────────────────────────────────────────────────────────────
/// Consumer
/// ─────────────────────────────────────────────────────────────────────────

/// Состояние читателя. Приватная память процесса. В shmem читателя нет вообще:
/// писатель на него не смотрит и о его существовании не знает.
struct spmc_ring_reader_local_state_t {
    /// Какое сообщение ждём. Единственное, что нужно для работы.
    std::uint64_t expected_message_number{0};

    /// Телеметрия
    std::uint64_t received_messages{0};
    std::uint64_t lost_messages{0};

    spmc_ring_resync_policy_t resync_policy{spmc_ring_resync_policy_t::resync_to_newest};

    /// ── Копия геометрии, снятая при attach ───────────────────────────
    /// Эти поля неизменны после конструирования, поэтому читать их из
    /// shmem на каждом try_read незачем. После исправления фолш-шаринга
    /// их строка чистая и лежит в S у всех читателей, но локальная копия
    /// убирает из горячего пути последнее обращение к разделяемой памяти
    /// кроме самого штампа.
    ///
    /// Цена: кэш устареет, если писатель пересоздаст кольцо под тем же
    /// адресом — в том числе с ДРУГИМ размером кольца. Ровно это и ловит
    /// сверка session_id ниже.
    std::uint64_t cached_ring_mask{0};
    std::uint64_t cached_stride{0};
    std::uint64_t cached_slots_offset{0};
    std::uint64_t cached_slot_capacity{0};

    /// Копия session_id, снятая при подключении.
    std::uint64_t cached_session_id{0};

    /// Приватный счётчик холостых проходов. Сверка сессии — обращение к
    /// разделяемой строке, и в цикле ожидания она давала бы пинг-понг,
    /// поэтому выполняется раз в session_check_interval холостых попыток.
    /// Счётчик приватный, поэтому сам троттлинг не стоит ничего.
    std::uint64_t idle_polls{0};
};

/// Через сколько холостых попыток чтения сверять session_id.
/// Степень двойки: проверка сводится к маске.
inline constexpr std::uint64_t session_check_interval = 256;


/// Адрес слота по локальной копии геометрии, без чтения хедера.
[[nodiscard]] inline data_slot_t*
spmc_ring_slot_cached(spmc_ring_buffer_t* self,
                      const spmc_ring_reader_local_state_t* state,
                      std::uint64_t message_number) noexcept {
    const std::uint64_t idx = message_number & state->cached_ring_mask;
    std::byte* base = reinterpret_cast<std::byte*>(self) + state->cached_slots_offset;
    return reinterpret_cast<data_slot_t*>(base + idx * state->cached_stride);
}


/// Подключение к готовому кольцу. Магия — барьер готовности: до неё поля
/// хедера читать нельзя, там могут быть нули недостроенного сегмента.
///
/// Не спинит: возвращает ring_is_not_ready, крутиться — дело вызывающего.
[[nodiscard]] inline spmc_ring_error_t
spmc_ring_reader_attach(spmc_ring_buffer_t* self,
                        spmc_ring_reader_local_state_t* state) noexcept {
    if (self == NULL || state == NULL) [[unlikely]] {
        return spmc_ring_error_t::reader_state_is_null;
    }

    const std::uint64_t magic = std::atomic_ref<std::uint64_t>{self->magic_number}
                                    .load(std::memory_order_acquire);
    if (magic != spmc_ring_buffer_t::magic_number_value) {
        return spmc_ring_error_t::ring_is_not_ready;
    }

    /// Магия прошла — остальные поля хедера теперь достоверны
    if (self->major_version != 0) [[unlikely]] {
        return spmc_ring_error_t::unsupported_major_version;
    }

    /// Раскладка сверяется ДО того, как мы начнём верить смещениям:
    /// при расхождении ABI все последующие поля читались бы не оттуда.
    if (self->layout_hash != spmc_layout_hash_value) [[unlikely]] {
        return spmc_ring_error_t::layout_mismatch;
    }

    /// Стартуем с текущей головы: истории не хотим, хотим свежее
    std::uint64_t head = std::atomic_ref<std::uint64_t>{self->global_message_number}
                             .load(std::memory_order_acquire);
    if (head == 0) {
        head = 1;   /// ноль занят под «слот пуст»
    }

    state->expected_message_number = head;
    state->received_messages = 0;
    state->lost_messages = 0;

    state->cached_session_id     = std::atomic_ref<std::uint64_t>{self->session_id}
                                       .load(std::memory_order_acquire);
    state->idle_polls            = 0;

    state->cached_ring_mask      = self->ring_size_pow_2 - 1;
    state->cached_stride         = self->single_slot_stride;
    state->cached_slots_offset   = self->data_slots_array_offset;
    state->cached_slot_capacity  = self->single_slot_stride - offsetof(data_slot_t, payload);

    return spmc_ring_error_t::no_error;
}


/// Пересчитывает expected после обгона и возвращает число потерянных.
///
/// observed_lower_bound — номер, про который точно известно, что писатель
/// его уже достиг (мы только что видели его в слоте). Нужен для гарантии
/// прогресса: global_message_number обновляется раз в батч и может отставать
/// настолько, что прыжок «в голову» не сдвинет expected вообще, и читатель
/// залипнет на обгоне навечно.
inline std::uint64_t
spmc_ring_resync(spmc_ring_buffer_t* self,
                 spmc_ring_reader_local_state_t* state,
                 std::uint64_t observed_lower_bound) noexcept {
    const std::uint64_t head = std::atomic_ref<std::uint64_t>{self->global_message_number}
                                   .load(std::memory_order_acquire);
    const std::uint64_t ring_size = self->ring_size_pow_2;

    std::uint64_t target;
    if (state->resync_policy == spmc_ring_resync_policy_t::resync_to_oldest_alive) {
        /// Слот head - ring_size писатель затрёт следующим действием,
        /// поэтому безопасная граница окна на единицу выше.
        target = (head > ring_size) ? (head - ring_size + 1) : 1;
    } else {
        target = head;
    }

    if (target <= state->expected_message_number) {
        target = observed_lower_bound;
    }

    const std::uint64_t lost = target - state->expected_message_number;
    state->lost_messages += lost;
    state->expected_message_number = target;

    return lost;
}


/// Одна попытка прочитать следующее сообщение.
///
/// Копия обязательна: проверка после чтения валидирует то, что лежит
/// в out_buffer, а не то, что осталось в слоте. Отдать наружу указатель
/// внутрь слота — значит выбросить всю защиту от обгона.
///
/// Ресинк выполняется внутри, поэтому expected двигается всегда и залипнуть
/// на обгоне невозможно. Число потерянных выкладывается в out_lost — мимо
/// него не пройти, в отличие от кода возврата, который легко проигнорировать.
[[nodiscard]] inline spmc_ring_error_t
spmc_ring_try_read(spmc_ring_buffer_t* self,
                   spmc_ring_reader_local_state_t* state,
                   void* out_buffer, std::uint64_t out_capacity,
                   std::uint64_t* out_bytes, std::uint64_t* out_lost) noexcept {
    if (self == NULL || state == NULL || out_bytes == NULL || out_lost == NULL) [[unlikely]] {
        return spmc_ring_error_t::reader_state_is_null;
    }

    *out_bytes = 0;
    *out_lost = 0;

    const std::uint64_t expected = state->expected_message_number;

    data_slot_t* const slot = spmc_ring_slot_cached(self, state, expected);
    std::atomic_ref<std::uint64_t> number{slot->contained_message_number};

    /// ── Проверка ДО ────────────────────────────────────────────────────
    /// Acquire: чтение payload не должно всплыть выше этой загрузки, иначе
    /// проверка проверяла бы не те байты, которые мы потом скопируем.
    const std::uint64_t before = number.load(std::memory_order_acquire);

    if (before & stamp_in_progress) {
        return spmc_ring_error_t::writer_is_inside_slot;
    }

    const std::uint64_t contained = before >> stamp_seq_shift;

    if (contained < expected) {
        /// Слот держит сообщение предыдущего круга — писатель до нас не дошёл.
        /// Отдельного флага «опубликовано» не нужно, его роль играет сравнение.
        ///
        /// Здесь же единственное место, где может проявиться перезапуск
        /// писателя: после пересоздания кольца наш expected выше любого
        /// штампа в нём, и это состояние НЕОТЛИЧИМО от тишины на фиде.
        /// Поэтому раз в session_check_interval холостых проходов сверяем
        /// сессию. На успешном пути чтения проверки нет вообще.
        if ((++state->idle_polls & (session_check_interval - 1)) == 0) [[unlikely]] {
            const std::uint64_t now_session =
                std::atomic_ref<std::uint64_t>{self->session_id}
                    .load(std::memory_order_acquire);
            if (now_session != state->cached_session_id) {
                /// Нумерация, размер кольца и геометрия могли поменяться —
                /// кэш читателя невалиден целиком. Чинится только повторным
                /// spmc_ring_reader_attach, поэтому решение за вызывающим.
                return spmc_ring_error_t::session_changed;
            }
        }
        return spmc_ring_error_t::message_not_published_yet;
    }

    if (contained > expected) {
        *out_lost = spmc_ring_resync(self, state, contained);
        return spmc_ring_error_t::message_was_lapped;
    }

    /// ── Копия ──────────────────────────────────────────────────────────
    /// Длина берётся из геометрии слота, а не из его содержимого — размер
    /// в слоте больше не хранится. Клампы оставлены: out_capacity приходит
    /// от вызывающего и может быть меньше слота, тогда отдаём префикс.
    SPMC_TEST_HOOK_BEFORE_COPY();

    const std::uint64_t slot_capacity = state->cached_slot_capacity;

    std::uint64_t copy_size = slot_capacity;
    if (copy_size > out_capacity) copy_size = out_capacity;

    std::memcpy(out_buffer, &slot->payload, copy_size);

    /// ── Проверка ПОСЛЕ ─────────────────────────────────────────────────
    /// За время memcpy писатель мог пройти полный круг и начать затирать
    /// этот слот. Fence, а не acquire на загрузке: нужен LoadLoad, чтобы
    /// перечитывание не уехало выше копирования.
    std::atomic_thread_fence(std::memory_order_acquire);

    const std::uint64_t after = number.load(std::memory_order_relaxed);
    if (after != before) {
        *out_lost = spmc_ring_resync(self, state, after >> stamp_seq_shift);
        return spmc_ring_error_t::message_was_lapped;
    }

    /// Штамп не менялся, значит копия валидна. Если тело слота не влезло
    /// целиком — сообщение на месте, expected НЕ двигаем, вызывающий может
    /// перечитать с буфером побольше.
    if (slot_capacity > out_capacity) [[unlikely]] {
        return spmc_ring_error_t::out_buffer_too_small;
    }

    state->expected_message_number = expected + 1;
    state->received_messages += 1;
    state->idle_polls = 0;

    /// Пустая запись — след отката писателя. Признак в штампе, а не в теле,
    /// поэтому он уже провалидирован обеими проверками: рваное значение
    /// не может быть принято за валидный флаг.
    if (before & stamp_empty) {
        *out_bytes = 0;
        return spmc_ring_error_t::empty_record;
    }

    *out_bytes = copy_size;
    return spmc_ring_error_t::no_error;
}


/// Читает до max_records подряд идущих сообщений за один вызов.
///
/// out_array трактуется как массив записей по record_stride байт. Чтение
/// останавливается на первом исходе, отличном от успеха; причина остановки
/// возвращается, если не прочитано ни одного сообщения.
///
/// Потери суммируются по всей пачке: ресинк внутри батча не прерывает его,
/// потому что после прыжка следующие сообщения могут быть уже доступны.
///
/// В отличие от наивного читателя, выигрыш здесь скромный: опрос
/// global_message_number и запись курсора из горячего пути уже убраны,
/// а геометрия лежит в локальной копии. Остаётся амортизация проверок
/// аргументов и накладных вызова.
[[nodiscard]] inline spmc_ring_error_t
spmc_ring_try_read_batch(spmc_ring_buffer_t* self,
                         spmc_ring_reader_local_state_t* state,
                         void* out_array, std::uint64_t record_stride,
                         std::uint64_t max_records,
                         std::uint64_t* out_count, std::uint64_t* out_lost) noexcept {
    if (self == NULL || state == NULL || out_count == NULL || out_lost == NULL) [[unlikely]] {
        return spmc_ring_error_t::reader_state_is_null;
    }

    *out_count = 0;
    *out_lost = 0;

    std::byte* dst = static_cast<std::byte*>(out_array);
    spmc_ring_error_t last = spmc_ring_error_t::no_error;

    for (std::uint64_t n = 0; n < max_records; ++n) {
        std::uint64_t bytes = 0, lost = 0;
        last = spmc_ring_try_read(self, state, dst + n * record_stride,
                                  record_stride, &bytes, &lost);
        *out_lost += lost;

        if (last == spmc_ring_error_t::no_error || last == spmc_ring_error_t::empty_record) {
            ++(*out_count);
            continue;
        }
        if (last == spmc_ring_error_t::message_was_lapped) {
            /// Ресинк уже произошёл, expected сдвинут — пробуем дальше
            continue;
        }
        break;
    }

    return (*out_count > 0) ? spmc_ring_error_t::no_error : last;
}
