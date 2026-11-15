#pragma once

#include "population/tape.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

namespace population::test {

// In-memory Tape for unit tests (no JSONL).
class FakeTape final : public Tape {
public:
    explicit FakeTape(std::vector<SnapshotEvent> events) : events_(std::move(events)) {
        if (events_.empty()) {
            throw std::invalid_argument("FakeTape: empty events");
        }
    }

    [[nodiscard]] std::uint64_t StartTs() const override { return events_.front().ts_ns; }

    [[nodiscard]] const std::string& FirstSymbol() const override {
        return events_.front().symbol;
    }

    [[nodiscard]] std::optional<SnapshotEvent> Next() override {
        if (index_ >= events_.size()) {
            return std::nullopt;
        }
        return events_[index_++];
    }

private:
    std::vector<SnapshotEvent> events_;
    std::size_t index_ = 0;
};

}  // namespace population::test
