#pragma once

#include "population/tape.hpp"

#include <fstream>
#include <memory>
#include <optional>
#include <string>

namespace dataset {

// Adapter: converted snapshot JSONL → population::Tape.
class JsonlTape final : public population::Tape {
public:
    // If only_symbol is non-empty, lines for other symbols are skipped.
    explicit JsonlTape(std::string path, std::string only_symbol = {});

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    [[nodiscard]] std::uint64_t StartTs() const override;
    [[nodiscard]] const std::string& FirstSymbol() const override;
    [[nodiscard]] std::optional<population::SnapshotEvent> Next() override;
    [[nodiscard]] bool NextInto(population::SnapshotEvent& out) override;

private:
    [[nodiscard]] bool ReadOneInto(population::SnapshotEvent& out);

    std::string path_;
    std::string only_symbol_;
    std::ifstream file_;
    std::string line_;                       // reused by getline
    population::SnapshotEvent parse_event_;  // reused parse scratch (keeps capacity)
    population::SnapshotEvent pending_;      // first event / Next() scratch
    bool has_pending_ = false;
};

[[nodiscard]] inline population::TapePtr MakeJsonlTape(std::string path,
                                                       std::string only_symbol = {}) {
    return std::make_unique<JsonlTape>(std::move(path), std::move(only_symbol));
}

}  // namespace dataset
