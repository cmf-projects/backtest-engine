#include "dataset/feather_tape.hpp"

#include "population/snapshot_assign.hpp"
#include "population/types.hpp"

#include <arrow/array.h>
#include <arrow/io/file.h>
#include <arrow/ipc/reader.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <nlohmann/json.hpp>

#include <algorithm>
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

FeatherBboTape::FeatherBboTape(std::string path, std::string symbol)
    : path_(std::move(path)), symbol_(std::move(symbol)) {
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
        const auto reader = Unwrap(arrow::ipc::RecordBatchFileReader::Open(input), file);
        for (int b = 0; b < reader->num_record_batches(); ++b) {
            const auto batch = Unwrap(reader->ReadRecordBatch(b), file);
            const auto& ts = Column<arrow::UInt64Type>(*batch, "ts_recv", file);
            const auto& ids = Column<arrow::UInt32Type>(*batch, "instrument_id", file);
            const auto& bid_px = Column<arrow::Int64Type>(*batch, "bid_px_00", file);
            const auto& ask_px = Column<arrow::Int64Type>(*batch, "ask_px_00", file);
            const auto& bid_sz = Column<arrow::UInt32Type>(*batch, "bid_sz_00", file);
            const auto& ask_sz = Column<arrow::UInt32Type>(*batch, "ask_sz_00", file);
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
