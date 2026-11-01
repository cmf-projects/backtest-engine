#pragma once

#include "population/types.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace population {

// Pull iterator over a single-component dataset.
// Adapters live under dataset/ (JSONL today; binary / mmap later).
//
// Contract:
// - After construction there is a pending first event (empty dataset → throw).
// - StartTs() / FirstSymbol() refer to that pending event.
// - Each Next() yields one SnapshotEvent until EOF (nullopt).
class Tape {
public:
    virtual ~Tape() = default;

    [[nodiscard]] virtual std::uint64_t StartTs() const = 0;
    [[nodiscard]] virtual const std::string& FirstSymbol() const = 0;
    [[nodiscard]] virtual std::optional<SnapshotEvent> Next() = 0;
};

using TapePtr = std::unique_ptr<Tape>;

}  // namespace population
