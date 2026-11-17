#pragma once

#include "population/tape.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dataset {

struct DaySlabPool;

// Adapter: Databento bbo-1m Feather deliveries → population::Tape.
//
// Each part holds one top-of-book row per instrument per interval for many
// instruments; the tape keeps only `symbol` (Databento raw_symbol, e.g. "MGCZ5")
// and yields one snapshot per row: ts_ns = ts_recv (interval end), one level per
// side, a side is empty when Databento has no price for it.
//
// `path` may be:
//   - a part file (…/part-00000.feather), sidecars read from its directory;
//   - a delivery dir with manifest.json (+ source-metadata.json);
//   - any dir above that (e.g. data/ or data/20251113/): deliveries are replayed
//     in path name order, days without `symbol` are skipped.
class FeatherBboTape final : public population::Tape {
public:
    FeatherBboTape(std::string path, std::string symbol);
    ~FeatherBboTape() override;

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    [[nodiscard]] std::uint64_t StartTs() const override;
    [[nodiscard]] const std::string& FirstSymbol() const override;
    [[nodiscard]] std::optional<population::SnapshotEvent> Next() override;
    [[nodiscard]] bool NextInto(population::SnapshotEvent& out) override;

private:
    struct Row {
        std::uint64_t ts_ns = 0;
        std::int64_t bid_px = 0;
        std::int64_t ask_px = 0;
        std::uint32_t bid_sz = 0;
        std::uint32_t ask_sz = 0;
    };

    [[nodiscard]] bool LoadNextPart();
    [[nodiscard]] bool ReadOneInto(population::SnapshotEvent& out);

    std::string path_;
    std::string symbol_;
    // One decompress buffer for every part. Starts at 2× the first day and
    // grows by 2× the shortage when a later day does not fit.
    std::unique_ptr<DaySlabPool> slab_;
    std::vector<std::filesystem::path> parts_;
    std::size_t next_part_ = 0;
    std::vector<Row> rows_;  // current part, `symbol_` only
    std::size_t row_ = 0;
    double price_scale_ = 1.0;
    std::uint64_t seq_ = 0;
    population::SnapshotEvent pending_;
    bool has_pending_ = false;
};

[[nodiscard]] inline population::TapePtr MakeFeatherBboTape(std::string path, std::string symbol) {
    return std::make_unique<FeatherBboTape>(std::move(path), std::move(symbol));
}

}  // namespace dataset
