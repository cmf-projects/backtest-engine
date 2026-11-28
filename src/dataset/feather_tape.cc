#include "dataset/feather_tape.hpp"

#include "hot_log.hpp"

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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace dataset {

namespace fs = std::filesystem;

constexpr std::string_view kSchemaBbo = "databento_bbo_1m";
constexpr std::string_view kSchemaMbp = "databento_mbp";

// Databento UNDEF_PRICE: no resting order on that side during the interval.
constexpr std::int64_t kUndefPrice = std::numeric_limits<std::int64_t>::max();

using population::Level;
using population::SnapshotEvent;

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

struct FeatherBboTape::DirMeta {
    double price_scale = 0.0;
    std::vector<std::string> filenames;
    std::vector<std::string> symbols;
    std::unordered_map<std::uint32_t, std::string> id_to_symbol;
};

std::shared_ptr<const FeatherBboTape::DirMeta> FeatherBboTape::LoadDir(const fs::path& dir) {
    const fs::path manifest_path = dir / "manifest.json";
    const fs::path metadata_path = dir / "source-metadata.json";
    const auto manifest = ReadJson(manifest_path);
    const auto metadata = ReadJson(metadata_path);
    auto info = std::make_shared<FeatherBboTape::DirMeta>();
    try {
        const auto schema = manifest.at("schema_name").get<std::string>();
        if (schema != kSchemaBbo && schema != kSchemaMbp) {
            throw std::runtime_error("unsupported schema '" + schema + "' in " +
                                     manifest_path.string() + " (want " + std::string(kSchemaBbo) +
                                     " or " + std::string(kSchemaMbp) + ")");
        }
        info->price_scale = manifest.at("price_scale").get<double>();
        std::unordered_set<std::string> seen;
        for (const auto& mapping : metadata.at("mappings")) {
            const auto symbol = mapping.at("raw_symbol").get<std::string>();
            if (seen.insert(symbol).second) {
                info->symbols.push_back(symbol);
            }
            for (const auto& interval : mapping.at("intervals")) {
                info->id_to_symbol.emplace(
                    static_cast<std::uint32_t>(std::stoul(interval.at("symbol").get<std::string>())),
                    symbol);
            }
        }
        std::sort(info->symbols.begin(), info->symbols.end());
        if (!(info->price_scale > 0.0)) {
            throw std::runtime_error("bad price_scale in " + manifest_path.string());
        }
        for (const auto& part : manifest.at("parts")) {
            info->filenames.push_back(part.at("filename").get<std::string>());
        }
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error("bad sidecar in " + dir.string() + ": " + e.what());
    }
    return info;
}

std::shared_ptr<const FeatherBboTape::DirMeta> FeatherBboTape::CachedDir(const fs::path& dir) {
    static std::mutex mu;
    static std::unordered_map<std::string, std::shared_ptr<const FeatherBboTape::DirMeta>> cache;
    const std::string key = dir.string();
    std::lock_guard lock(mu);
    const auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }
    return cache.emplace(key, LoadDir(dir)).first->second;
}

void FeatherBboTape::CollectInto(const fs::path& dir, std::vector<ListedPart>& out) {
    const fs::path manifest_path = dir / "manifest.json";
    if (!fs::is_regular_file(manifest_path)) {
        std::vector<fs::path> subdirs;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.is_directory()) {
                subdirs.push_back(entry.path());
            }
        }
        std::sort(subdirs.begin(), subdirs.end());
        for (const auto& sub : subdirs) {
            CollectInto(sub, out);
        }
        return;
    }

    const std::shared_ptr<const DirMeta> meta = CachedDir(dir);
    for (const std::string& filename : meta->filenames) {
        out.push_back(ListedPart{.file = dir / filename, .meta = meta});
    }
}

void FeatherBboTape::CollectParts(const fs::path& dir) { CollectInto(dir, parts_); }

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

FeatherBboTape::FeatherBboTape(std::string path, std::string symbol)
    : path_(std::move(path)), symbol_(std::move(symbol)) {
    if (symbol_.empty()) {
        throw std::invalid_argument(
            "feather dataset needs a symbol (Databento raw_symbol, e.g. MGCZ5): " + path_);
    }
    const fs::path root(path_);
    if (fs::is_directory(root)) {
        CollectParts(root);
    } else if (fs::is_regular_file(root)) {
        CollectParts(root.parent_path());
        std::erase_if(parts_, [&](const ListedPart& part) { return part.file != root; });
    } else {
        throw std::runtime_error("cannot open dataset: " + path_);
    }
    if (parts_.empty()) {
        throw std::runtime_error("no feather parts (manifest.json) under: " + path_);
    }
    const RowCursor start = FirstRow(path_, parts_, symbol_);
    stream_ = std::make_unique<Stream>(parts_, symbol_, start);
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

struct OpenPart {
    static constexpr int kCap = 64;

    fs::path file;
    std::shared_ptr<arrow::io::ReadableFile> input;
    std::shared_ptr<arrow::ipc::RecordBatchFileReader> reader;
    std::shared_ptr<arrow::RecordBatch> batch;
    int shown = -1;
    int depth = 0;
    const arrow::UInt64Array* ts = nullptr;
    const arrow::UInt32Array* ids = nullptr;
    const arrow::Int64Array* bid_px[kCap]{};
    const arrow::Int64Array* ask_px[kCap]{};
    const arrow::UInt32Array* bid_sz[kCap]{};
    const arrow::UInt32Array* ask_sz[kCap]{};

    static OpenPart& Get(const fs::path& path) {
        static std::mutex mu;
        static std::unordered_map<std::string, std::unique_ptr<OpenPart>> open;
        std::lock_guard lock(mu);
        const std::string key = path.string();
        std::unique_ptr<OpenPart>& slot = open[key];
        if (slot == nullptr) {
            slot = std::make_unique<OpenPart>();
            slot->Open(path);
        }
        return *slot;
    }

    void Open(const fs::path& path) {
        file = path;
        input = Unwrap(arrow::io::ReadableFile::Open(path.string()), path);
        arrow::ipc::IpcReadOptions options;
        options.use_threads = false;
        reader = Unwrap(arrow::ipc::RecordBatchFileReader::Open(input, options), path);
    }

    void Show(int batch_no) {
        if (shown == batch_no && batch != nullptr) {
            return;
        }
        batch.reset();
        batch = Unwrap(reader->ReadRecordBatch(batch_no), file);
        shown = batch_no;
        ts = &Column<arrow::UInt64Type>(*batch, "ts_recv", file);
        ids = &Column<arrow::UInt32Type>(*batch, "instrument_id", file);
        depth = 0;
        for (; depth < kCap; ++depth) {
            char bid_px_name[16];
            char ask_px_name[16];
            char bid_sz_name[16];
            char ask_sz_name[16];
            std::snprintf(bid_px_name, sizeof(bid_px_name), "bid_px_%02d", depth);
            std::snprintf(ask_px_name, sizeof(ask_px_name), "ask_px_%02d", depth);
            std::snprintf(bid_sz_name, sizeof(bid_sz_name), "bid_sz_%02d", depth);
            std::snprintf(ask_sz_name, sizeof(ask_sz_name), "ask_sz_%02d", depth);
            if (batch->GetColumnByName(bid_px_name) == nullptr) {
                break;
            }
            bid_px[depth] = &Column<arrow::Int64Type>(*batch, bid_px_name, file);
            ask_px[depth] = &Column<arrow::Int64Type>(*batch, ask_px_name, file);
            bid_sz[depth] = &Column<arrow::UInt32Type>(*batch, bid_sz_name, file);
            ask_sz[depth] = &Column<arrow::UInt32Type>(*batch, ask_sz_name, file);
        }
        if (depth == 0) {
            throw std::runtime_error("arrow: " + file.string() + ": no bid_px_00 column");
        }
    }
};

FeatherBboTape::RowCursor FeatherBboTape::FirstRow(const std::string& key,
                                                      const std::vector<ListedPart>& parts,
                                                      const std::string& symbol) {
    struct Index {
        std::unordered_map<std::string, RowCursor> at;
    };
    static std::mutex mu;
    static std::unordered_map<std::string, Index> cache;
    std::lock_guard lock(mu);
    const auto existing = cache.find(key);
    if (existing == cache.end()) {
        Index built;
        std::unordered_set<std::string> waiting;
        for (const ListedPart& part : parts) {
            if (part.meta == nullptr) {
                continue;
            }
            for (const std::string& name : part.meta->symbols) {
                waiting.insert(name);
            }
        }
        for (std::size_t p = 0; p < parts.size() && !waiting.empty(); ++p) {
            const ListedPart& part = parts[p];
            if (part.meta == nullptr) {
                continue;
            }
            OpenPart& file = OpenPart::Get(part.file);
            if (file.reader->num_record_batches() <= 0) {
                continue;
            }
            for (int b = 0; b < file.reader->num_record_batches() && !waiting.empty(); ++b) {
                file.Show(b);
                const std::int64_t rows = file.batch->num_rows();
                for (std::int64_t r = 0; r < rows && !waiting.empty(); ++r) {
                    const auto id = part.meta->id_to_symbol.find(file.ids->Value(r));
                    if (id == part.meta->id_to_symbol.end()) {
                        continue;
                    }
                    if (waiting.erase(id->second) == 0) {
                        continue;
                    }
                    built.at.emplace(id->second, RowCursor{.part = p, .batch = b, .row = r, .found = true});
                }
            }
        }
        cache.emplace(key, std::move(built));
    }
    const auto at = cache.find(key)->second.at.find(symbol);
    if (at == cache.find(key)->second.at.end()) {
        return {};
    }
    return at->second;
}

struct FeatherBboTape::Stream {
    explicit Stream(const std::vector<ListedPart>& parts, const std::string& symbol, RowCursor start)
        : parts_(&parts),
          all_(symbol.empty()),
          part_(all_ || start.found ? start.part : parts.size()),
          batch_(start.batch),
          row_(start.row) {
        if (all_) {
            return;
        }
        for (const ListedPart& part : parts) {
            if (part.meta == nullptr) {
                continue;
            }
            for (const auto& [id, name] : part.meta->id_to_symbol) {
                if (name == symbol) {
                    ids_.insert(id);
                }
            }
        }
    }

    [[nodiscard]] bool Next(std::uint64_t& seq, const std::string& symbol, population::SnapshotEvent& out) {
#if CMF_HOT_LOG
        const auto t0 = std::chrono::steady_clock::now();
#endif
        const bool ok = NextBody(seq, symbol, out);
#if CMF_HOT_LOG
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        static std::uint64_t acc_ns = 0;
        static std::uint64_t acc_n = 0;
        static auto last = std::chrono::steady_clock::now();
        acc_ns += static_cast<std::uint64_t>(ns);
        ++acc_n;
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(5)) {
            std::fprintf(stderr, "hot feather ms=%.0f calls=%llu\n", acc_ns / 1e6,
                         static_cast<unsigned long long>(acc_n));
            acc_ns = 0;
            acc_n = 0;
            last = now;
        }
#endif
        return ok;
    }

    [[nodiscard]] bool NextBody(std::uint64_t& seq, const std::string& symbol, population::SnapshotEvent& out) {
        while (part_ < parts_->size()) {
            const ListedPart& part = (*parts_)[part_];
            OpenPart& file = OpenPart::Get(part.file);
            if (file.reader->num_record_batches() <= 0) {
                ++part_;
                batch_ = 0;
                row_ = 0;
                continue;
            }
            if (batch_ >= file.reader->num_record_batches()) {
                ++part_;
                batch_ = 0;
                row_ = 0;
                continue;
            }
            file.Show(batch_);
            if (row_ >= file.batch->num_rows()) {
                ++batch_;
                row_ = 0;
                continue;
            }
            const std::uint32_t id = file.ids->Value(row_);
            std::string row_symbol = symbol;
            if (all_) {
                if (part.meta == nullptr) {
                    ++row_;
                    continue;
                }
                const auto found = part.meta->id_to_symbol.find(id);
                if (found == part.meta->id_to_symbol.end()) {
                    ++row_;
                    continue;
                }
                row_symbol = found->second;
            } else if (!ids_.contains(id)) {
                ++row_;
                continue;
            }
            out.seq = ++seq;
            out.ts_ns = file.ts->Value(row_);
            out.symbol = std::move(row_symbol);
            out.bids.clear();
            out.asks.clear();
            const double scale = part.meta->price_scale;
            for (int lvl = 0; lvl < file.depth; ++lvl) {
                const auto px = file.bid_px[lvl]->Value(row_);
                const auto sz = file.bid_sz[lvl]->Value(row_);
                if (px == kUndefPrice || sz == 0) {
                    break;
                }
                out.bids.push_back(population::Level{.price = static_cast<double>(px) / scale, .qty = sz});
            }
            for (int lvl = 0; lvl < file.depth; ++lvl) {
                const auto px = file.ask_px[lvl]->Value(row_);
                const auto sz = file.ask_sz[lvl]->Value(row_);
                if (px == kUndefPrice || sz == 0) {
                    break;
                }
                out.asks.push_back(population::Level{.price = static_cast<double>(px) / scale, .qty = sz});
            }
            ++row_;
            return true;
        }
        return false;
    }

    const std::vector<ListedPart>* parts_ = nullptr;
    bool all_ = false;
    std::unordered_set<std::uint32_t> ids_;
    std::size_t part_ = 0;
    int batch_ = 0;
    std::int64_t row_ = 0;
};

FeatherBboTape::~FeatherBboTape() = default;

bool FeatherBboTape::ReadOneInto(SnapshotEvent& out) {
    return stream_->Next(seq_, symbol_, out);
}

struct FeatherJournal::Body {
    std::vector<FeatherBboTape::ListedPart> parts;
    std::unique_ptr<FeatherBboTape::Stream> stream;
    std::uint64_t seq = 0;
};

FeatherJournal::FeatherJournal(std::string path) : body_(std::make_unique<Body>()) {
    FeatherBboTape::CollectInto(fs::path(path), body_->parts);
    if (body_->parts.empty()) {
        throw std::runtime_error("no feather parts (manifest.json) under: " + path);
    }
    body_->stream = std::make_unique<FeatherBboTape::Stream>(body_->parts, "", FeatherBboTape::RowCursor{});
}

FeatherJournal::~FeatherJournal() = default;

bool FeatherJournal::Next(std::string& symbol, population::SnapshotEvent& out) {
    if (!body_->stream->Next(body_->seq, "", out)) {
        return false;
    }
    symbol = out.symbol;
    return true;
}

std::vector<std::string> ListFeatherSymbols(const std::string& path) {
    std::vector<std::string> symbols;
    std::unordered_set<std::string> seen;
    const auto take = [&](const fs::path& dir) {
        const auto meta = FeatherBboTape::CachedDir(dir);
        for (const std::string& symbol : meta->symbols) {
            if (seen.insert(symbol).second) {
                symbols.push_back(symbol);
            }
        }
    };
    const std::function<void(const fs::path&)> walk = [&](const fs::path& dir) {
        if (fs::is_regular_file(dir / "manifest.json")) {
            take(dir);
            return;
        }
        if (!fs::is_directory(dir)) {
            return;
        }
        std::vector<fs::path> subdirs;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.is_directory()) {
                subdirs.push_back(entry.path());
            }
        }
        std::sort(subdirs.begin(), subdirs.end());
        for (const fs::path& sub : subdirs) {
            walk(sub);
        }
    };
    const fs::path root(path);
    if (fs::is_regular_file(root)) {
        walk(root.parent_path());
    } else {
        walk(root);
    }
    std::sort(symbols.begin(), symbols.end());
    return symbols;
}

}  // namespace dataset
