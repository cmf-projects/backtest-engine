#include "dataset/feather_tape.hpp"

#include "population/snapshot_assign.hpp"
#include "population/types.hpp"

#include <arrow/array.h>
#include <arrow/io/file.h>
#include <arrow/ipc/options.h>
#include <arrow/ipc/reader.h>
#include <arrow/memory_pool.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace dataset {
namespace {

namespace fs = std::filesystem;
using population::Level;
using population::SnapshotEvent;

constexpr std::string_view kSchemaName = "databento_bbo_1m";
// Databento UNDEF_PRICE: no resting order on that side during the interval.
constexpr std::int64_t kUndefPrice = std::numeric_limits<std::int64_t>::max();

struct PartInfo {
    double price_scale = 0.0;
    std::unordered_set<std::uint32_t> instrument_ids;  // ids mapped to the symbol
};

template <class T>
[[nodiscard]] T Unwrap(arrow::Result<T> result, const fs::path& file) {
    if (!result.ok()) {
        throw std::runtime_error("arrow: " + file.string() + ": " + result.status().ToString());
    }
    return std::move(result).ValueUnsafe();
}

// Non-null column of exactly ArrowType; the batch must outlive the returned reference.
template <class ArrowType>
[[nodiscard]] const arrow::NumericArray<ArrowType>& Column(const arrow::RecordBatch& batch,
                                                           const char* name,
                                                           const fs::path& file) {
    const auto column = batch.GetColumnByName(name);
    if (column == nullptr) {
        throw std::runtime_error("arrow: " + file.string() + ": no column '" + name + "'");
    }
    if (column->type_id() != ArrowType::type_id) {
        throw std::runtime_error("arrow: " + file.string() + ": column '" + name + "' is " +
                                 column->type()->ToString() + ", want " +
                                 ArrowType::type_name());
    }
    if (column->null_count() != 0) {
        throw std::runtime_error("arrow: " + file.string() + ": column '" + name +
                                 "' has nulls");
    }
    return static_cast<const arrow::NumericArray<ArrowType>&>(*column);
}

[[nodiscard]] nlohmann::json ReadJson(const fs::path& file) {
    std::ifstream in(file);
    if (!in) {
        throw std::runtime_error("cannot open dataset sidecar: " + file.string());
    }
    try {
        return nlohmann::json::parse(in);
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error("bad json " + file.string() + ": " + e.what());
    }
}

void CollectParts(const fs::path& dir, std::vector<fs::path>& parts) {
    const fs::path manifest_path = dir / "manifest.json";
    if (fs::is_regular_file(manifest_path)) {
        const auto manifest = ReadJson(manifest_path);
        try {
            for (const auto& part : manifest.at("parts")) {
                parts.push_back(dir / part.at("filename").get<std::string>());
            }
        } catch (const nlohmann::json::exception& e) {
            throw std::runtime_error("bad manifest " + manifest_path.string() + ": " + e.what());
        }
        return;
    }
    std::vector<fs::path> subdirs;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_directory()) {
            subdirs.push_back(entry.path());
        }
    }
    std::sort(subdirs.begin(), subdirs.end());
    for (const auto& sub : subdirs) {
        CollectParts(sub, parts);
    }
}

[[nodiscard]] PartInfo LoadPartInfo(const fs::path& part, const std::string& symbol) {
    const fs::path dir = part.parent_path();
    const fs::path manifest_path = dir / "manifest.json";
    const fs::path metadata_path = dir / "source-metadata.json";
    const auto manifest = ReadJson(manifest_path);
    const auto metadata = ReadJson(metadata_path);

    PartInfo info;
    try {
        const auto schema = manifest.at("schema_name").get<std::string>();
        if (schema != kSchemaName) {
            throw std::runtime_error("unsupported schema '" + schema + "' in " +
                                     manifest_path.string() + " (want " +
                                     std::string(kSchemaName) + ")");
        }
        info.price_scale = manifest.at("price_scale").get<double>();
        for (const auto& mapping : metadata.at("mappings")) {
            if (mapping.at("raw_symbol").get<std::string>() != symbol) {
                continue;
            }
            for (const auto& interval : mapping.at("intervals")) {
                // stype_out = instrument_id, written as a decimal string.
                info.instrument_ids.insert(
                    static_cast<std::uint32_t>(std::stoul(interval.at("symbol").get<std::string>())));
            }
        }
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error("bad sidecar in " + dir.string() + ": " + e.what());
    }
    if (!(info.price_scale > 0.0)) {
        throw std::runtime_error("bad price_scale in " + manifest_path.string());
    }
    return info;
}

}  // namespace

namespace {

constexpr int64_t kAlign = 64;

[[nodiscard]] int64_t AlignUp(int64_t n, int64_t alignment) {
    return (n + alignment - 1) & ~(alignment - 1);
}

[[nodiscard]] uint8_t* AllocAligned(int64_t bytes) {
    void* ptr = nullptr;
    if (posix_memalign(&ptr, static_cast<size_t>(kAlign), static_cast<size_t>(bytes)) != 0) {
        return nullptr;
    }
    return static_cast<uint8_t*>(ptr);
}

}  // namespace

// Bump allocator over one block. Arrow asks for each column separately, so a
// day that does not fit grows by a side block of 2× the shortage (live
// pointers cannot move). When the batch is released the side blocks are
// folded into one buffer.
class DaySlabPool final : public arrow::MemoryPool {
public:
    ~DaySlabPool() override { ReleaseAll(); }

    arrow::Status Allocate(int64_t size, int64_t alignment, uint8_t** out) override {
        if (size < 0 || alignment <= 0 || (alignment & (alignment - 1)) != 0) {
            return arrow::Status::Invalid("day slab allocate");
        }
        if (size == 0) {
            *out = Zero();
            return arrow::Status::OK();
        }
        if (uint8_t* ptr = TryBump(size, alignment)) {
        live_ += size;
        total_ += size;
        *out = ptr;
        return arrow::Status::OK();
        }
        const int64_t shortage = Shortage(size, alignment);
        // One pointer has to cover the whole request, so the new block is at
        // least `size`. 2× the gap is the spare on top of that when it fits.
        const int64_t extra = std::max(size, static_cast<int64_t>(2) * shortage);
        if (live_ == 0) {
            const int64_t next = std::max(capacity_ + 2 * shortage, extra);
            if (!ReplaceSlab(next)) {
                return arrow::Status::OutOfMemory("day slab");
            }
        } else if (!AddChunk(extra)) {
            return arrow::Status::OutOfMemory("day slab");
        }
        uint8_t* ptr = TryBump(size, alignment);
        if (ptr == nullptr) {
            return arrow::Status::OutOfMemory("day slab");
        }
        live_ += size;
        total_ += size;
        *out = ptr;
        return arrow::Status::OK();
    }

    arrow::Status Reallocate(int64_t old_size, int64_t new_size, int64_t alignment,
                             uint8_t** ptr) override {
        if (new_size <= old_size) {
            live_ -= old_size - new_size;
            return arrow::Status::OK();
        }
        uint8_t* grown = nullptr;
        auto status = Allocate(new_size, alignment, &grown);
        if (!status.ok()) {
            return status;
        }
        if (old_size > 0 && *ptr != nullptr && *ptr != Zero()) {
            std::memcpy(grown, *ptr, static_cast<size_t>(old_size));
        }
        Free(*ptr, old_size, alignment);
        *ptr = grown;
        return arrow::Status::OK();
    }

    void Free(uint8_t* buffer, int64_t size, int64_t alignment) override {
        (void)alignment;
        if (size <= 0 || buffer == nullptr || buffer == Zero()) {
            return;
        }
        live_ -= size;
        if (live_ > 0) {
            return;
        }
        live_ = 0;
        bump_ = 0;
        for (Chunk& chunk : extras_) {
            chunk.bump = 0;
        }
        if (!extras_.empty()) {
            Compact();
        }
    }

    int64_t bytes_allocated() const override { return live_; }
    int64_t max_memory() const override { return capacity_ + ExtraCapacity(); }
    int64_t total_bytes_allocated() const override { return total_; }
    int64_t num_allocations() const override { return grows_; }
    std::string backend_name() const override { return "day-slab"; }

private:
    struct Chunk {
        uint8_t* ptr = nullptr;
        int64_t cap = 0;
        int64_t bump = 0;
    };

    static uint8_t* Zero() {
        alignas(64) static uint8_t byte = 0;
        return &byte;
    }

    [[nodiscard]] int64_t ExtraCapacity() const {
        int64_t n = 0;
        for (const Chunk& chunk : extras_) {
            n += chunk.cap;
        }
        return n;
    }

    // Bytes past the end of the roomiest region. 0 if some region can take `size`.
    [[nodiscard]] int64_t Shortage(int64_t size, int64_t alignment) const {
        auto gap = [&](int64_t cap, int64_t bump) {
            const int64_t aligned = AlignUp(bump, alignment);
            if (aligned > cap) {
                return size;
            }
            const int64_t room = cap - aligned;
            return size > room ? size - room : static_cast<int64_t>(0);
        };
        int64_t best = gap(capacity_, bump_);
        for (const Chunk& chunk : extras_) {
            best = std::min(best, gap(chunk.cap, chunk.bump));
        }
        return best;
    }

    [[nodiscard]] uint8_t* TryBump(int64_t size, int64_t alignment) {
        if (uint8_t* ptr = Bump(slab_, capacity_, bump_, size, alignment)) {
            return ptr;
        }
        for (Chunk& chunk : extras_) {
            if (uint8_t* ptr = Bump(chunk.ptr, chunk.cap, chunk.bump, size, alignment)) {
                return ptr;
            }
        }
        return nullptr;
    }

    static uint8_t* Bump(uint8_t* base, int64_t cap, int64_t& bump, int64_t size,
                         int64_t alignment) {
        if (base == nullptr) {
            return nullptr;
        }
        const int64_t aligned = AlignUp(bump, alignment);
        if (aligned > cap || size > cap - aligned) {
            return nullptr;
        }
        bump = aligned + size;
        return base + aligned;
    }

    [[nodiscard]] bool ReplaceSlab(int64_t next) {
        if (next < capacity_) {
            return false;
        }
        uint8_t* ptr = AllocAligned(next);
        if (ptr == nullptr) {
            return false;
        }
        std::free(slab_);
        slab_ = ptr;
        capacity_ = next;
        bump_ = 0;
        ++grows_;
        return true;
    }

    [[nodiscard]] bool AddChunk(int64_t bytes) {
        const int64_t cap = AlignUp(bytes, kAlign);
        uint8_t* ptr = AllocAligned(cap);
        if (ptr == nullptr) {
            return false;
        }
        extras_.push_back(Chunk{.ptr = ptr, .cap = cap, .bump = 0});
        ++grows_;
        return true;
    }

    void Compact() {
        const int64_t total = capacity_ + ExtraCapacity();
        uint8_t* ptr = AllocAligned(total);
        if (ptr == nullptr) {
            bump_ = 0;
            return;
        }
        std::free(slab_);
        for (Chunk& chunk : extras_) {
            std::free(chunk.ptr);
        }
        extras_.clear();
        slab_ = ptr;
        capacity_ = total;
        bump_ = 0;
        ++grows_;
    }

    void ReleaseAll() {
        std::free(slab_);
        slab_ = nullptr;
        for (Chunk& chunk : extras_) {
            std::free(chunk.ptr);
        }
        extras_.clear();
    }

    uint8_t* slab_ = nullptr;
    int64_t capacity_ = 0;
    int64_t bump_ = 0;
    int64_t live_ = 0;
    int64_t total_ = 0;
    int64_t grows_ = 0;
    std::vector<Chunk> extras_;
};

FeatherBboTape::~FeatherBboTape() = default;

FeatherBboTape::FeatherBboTape(std::string path, std::string symbol)
    : path_(std::move(path)), symbol_(std::move(symbol)), slab_(std::make_unique<DaySlabPool>()) {
    if (symbol_.empty()) {
        throw std::invalid_argument(
            "feather dataset needs a symbol (Databento raw_symbol, e.g. MGCZ5): " + path_);
    }
    const fs::path root(path_);
    if (fs::is_directory(root)) {
        CollectParts(root, parts_);
    } else if (fs::is_regular_file(root)) {
        parts_.push_back(root);
    } else {
        throw std::runtime_error("cannot open dataset: " + path_);
    }
    if (parts_.empty()) {
        throw std::runtime_error("no feather parts (manifest.json) under: " + path_);
    }
    pending_.bids.reserve(1);
    pending_.asks.reserve(1);
    if (!ReadOneInto(pending_)) {
        throw std::runtime_error("dataset has no rows for symbol: " + symbol_ + " (" + path_ + ")");
    }
    has_pending_ = true;
}

std::uint64_t FeatherBboTape::StartTs() const {
    return pending_.ts_ns;
}

const std::string& FeatherBboTape::FirstSymbol() const {
    return symbol_;
}

bool FeatherBboTape::NextInto(SnapshotEvent& out) {
    if (has_pending_) {
        population::AssignSnapshot(out, pending_);
        has_pending_ = false;
        return true;
    }
    return ReadOneInto(out);
}

std::optional<SnapshotEvent> FeatherBboTape::Next() {
    SnapshotEvent event;
    if (!NextInto(event)) {
        return std::nullopt;
    }
    return event;
}

bool FeatherBboTape::LoadNextPart() {
    rows_.clear();
    row_ = 0;
    while (next_part_ < parts_.size()) {
        const fs::path& file = parts_[next_part_++];
        const PartInfo info = LoadPartInfo(file, symbol_);
        if (info.instrument_ids.empty()) {
            continue;
        }
        const auto input = Unwrap(arrow::io::ReadableFile::Open(file.string()), file);
        arrow::ipc::IpcReadOptions options;
        options.memory_pool = slab_.get();
        options.use_threads = false;
        const auto reader = Unwrap(arrow::ipc::RecordBatchFileReader::Open(input, options), file);
        for (int b = 0; b < reader->num_record_batches(); ++b) {
            const auto batch = Unwrap(reader->ReadRecordBatch(b), file);
            const auto& ts = Column<arrow::UInt64Type>(*batch, "ts_recv", file);
            const auto& ids = Column<arrow::UInt32Type>(*batch, "instrument_id", file);
            const auto& bid_px = Column<arrow::Int64Type>(*batch, "bid_px_00", file);
            const auto& ask_px = Column<arrow::Int64Type>(*batch, "ask_px_00", file);
            const auto& bid_sz = Column<arrow::UInt32Type>(*batch, "bid_sz_00", file);
            const auto& ask_sz = Column<arrow::UInt32Type>(*batch, "ask_sz_00", file);
            std::size_t keep = 0;
            for (std::int64_t i = 0; i < batch->num_rows(); ++i) {
                if (info.instrument_ids.contains(ids.Value(i))) {
                    ++keep;
                }
            }
            if (rows_.capacity() < keep) {
                const std::size_t shortage = keep - rows_.capacity();
                rows_.reserve(rows_.capacity() + shortage * 2);
            }
            for (std::int64_t i = 0; i < batch->num_rows(); ++i) {
                if (info.instrument_ids.contains(ids.Value(i))) {
                    rows_.push_back(Row{.ts_ns = ts.Value(i),
                                        .bid_px = bid_px.Value(i),
                                        .ask_px = ask_px.Value(i),
                                        .bid_sz = bid_sz.Value(i),
                                        .ask_sz = ask_sz.Value(i)});
                }
            }
        }
        if (!rows_.empty()) {
            price_scale_ = info.price_scale;
            return true;
        }
    }
    return false;
}

bool FeatherBboTape::ReadOneInto(SnapshotEvent& out) {
    while (row_ >= rows_.size()) {
        if (!LoadNextPart()) {
            return false;
        }
    }
    const Row& row = rows_[row_++];
    out.seq = ++seq_;
    out.ts_ns = row.ts_ns;
    out.symbol = symbol_;
    out.bids.clear();
    out.asks.clear();
    if (row.bid_px != kUndefPrice && row.bid_sz > 0) {
        out.bids.push_back(Level{.price = static_cast<double>(row.bid_px) / price_scale_,
                                 .qty = row.bid_sz});
    }
    if (row.ask_px != kUndefPrice && row.ask_sz > 0) {
        out.asks.push_back(Level{.price = static_cast<double>(row.ask_px) / price_scale_,
                                 .qty = row.ask_sz});
    }
    return true;
}

}  // namespace dataset
