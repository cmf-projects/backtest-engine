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

[[nodiscard]] std::vector<std::string> ListFeatherSymbols(const std::string& path);

// Adapter: Databento bbo-1m and mbp Feather deliveries → population::Tape.
//
// Each row is one snapshot. Columns bid_px_00 / ask_px_00 are the best prices.
// bid_px_01, bid_px_02, … are the deeper L2 levels when the file has them.
// A side ends at the first missing column or UNDEF price. Each tape yields one
// `symbol`. The first tape of a directory walks the files once and remembers
// where every symbol's first row sits. Later tapes of that directory start
// there. One Feather file keeps a single record batch in memory, shared by
// every tape of that file. The batch is dropped when a reader moves to the
// next one. ts_ns = ts_recv.
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
    struct Stream;
    friend std::vector<std::string> ListFeatherSymbols(const std::string& path);
    friend class FeatherJournal;

    struct DirMeta;

    struct ListedPart {
        std::filesystem::path file;
        std::shared_ptr<const DirMeta> meta;
    };

    [[nodiscard]] static std::shared_ptr<const DirMeta> LoadDir(const std::filesystem::path& dir);
    [[nodiscard]] static std::shared_ptr<const DirMeta> CachedDir(const std::filesystem::path& dir);

    struct RowCursor {
        std::size_t part = 0;
        int batch = 0;
        std::int64_t row = 0;
        bool found = false;
    };

    // One forward pass per dataset path. Stops once every listed symbol has a
    // first row. Symbols with no rows stay found=false.
    [[nodiscard]] static RowCursor FirstRow(const std::string& key, const std::vector<ListedPart>& parts,
                                            const std::string& symbol);

    static void CollectInto(const std::filesystem::path& dir, std::vector<ListedPart>& out);
    void CollectParts(const std::filesystem::path& dir);
    [[nodiscard]] bool ReadOneInto(population::SnapshotEvent& out);

    std::string path_;
    std::string symbol_;
    std::vector<ListedPart> parts_;
    std::unique_ptr<Stream> stream_;
    double price_scale_ = 1.0;
    std::uint64_t seq_ = 0;
    population::SnapshotEvent pending_;
    bool has_pending_ = false;
};

[[nodiscard]] inline population::TapePtr MakeFeatherBboTape(std::string path, std::string symbol) {
    return std::make_unique<FeatherBboTape>(std::move(path), std::move(symbol));
}

// One forward cursor over every symbol in a delivery. No per-symbol seek:
// a row is yielded when the scan reaches it.
class FeatherJournal {
public:
    explicit FeatherJournal(std::string path);
    ~FeatherJournal();

    [[nodiscard]] bool Next(std::string& symbol, population::SnapshotEvent& out);

private:
    struct Body;
    std::unique_ptr<Body> body_;
};

}  // namespace dataset
