#include "dataset/jsonl_tape.hpp"

#include "population/types.hpp"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace dataset {
namespace {

using population::kMaxLevelsPerSide;
using population::Level;
using population::SnapshotEvent;

[[nodiscard]] std::optional<std::size_t> FindKeyColon(std::string_view line,
                                                      std::string_view key) {
    char pattern[96];
    const int n = std::snprintf(pattern, sizeof(pattern), "\"%.*s\":",
                                static_cast<int>(key.size()), key.data());
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(pattern)) {
        return std::nullopt;
    }
    const auto pos = line.find(std::string_view(pattern, static_cast<std::size_t>(n)));
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    return pos + static_cast<std::size_t>(n);
}

[[nodiscard]] std::optional<std::string_view> JsonStringField(std::string_view line,
                                                              std::string_view key) {
    char pattern[96];
    const int n = std::snprintf(pattern, sizeof(pattern), "\"%.*s\":\"",
                                static_cast<int>(key.size()), key.data());
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(pattern)) {
        return std::nullopt;
    }
    const auto pos = line.find(std::string_view(pattern, static_cast<std::size_t>(n)));
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    const auto start = pos + static_cast<std::size_t>(n);
    const auto end = line.find('"', start);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    return line.substr(start, end - start);
}

[[nodiscard]] std::optional<std::uint64_t> JsonU64Field(std::string_view line,
                                                        std::string_view key) {
    const auto start = FindKeyColon(line, key);
    if (!start) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto* begin = line.data() + *start;
    const auto* end = line.data() + line.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr == begin) {
        return std::nullopt;
    }
    return value;
}

void JsonLevelsInto(std::string_view line, std::string_view key, std::vector<Level>& levels) {
    levels.clear();
    if (levels.capacity() < kMaxLevelsPerSide) {
        levels.reserve(kMaxLevelsPerSide);
    }
    char pattern[96];
    const int n = std::snprintf(pattern, sizeof(pattern), "\"%.*s\":[",
                                static_cast<int>(key.size()), key.data());
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(pattern)) {
        return;
    }
    const auto pos = line.find(std::string_view(pattern, static_cast<std::size_t>(n)));
    if (pos == std::string_view::npos) {
        return;
    }
    auto i = pos + static_cast<std::size_t>(n);
    while (i < line.size() && line[i] != ']') {
        while (i < line.size() && (line[i] == ' ' || line[i] == ',')) {
            ++i;
        }
        if (i < line.size() && line[i] == ']') {
            break;
        }
        if (i >= line.size() || line[i] != '{') {
            break;
        }
        const auto obj_end = line.find('}', i);
        if (obj_end == std::string_view::npos) {
            break;
        }
        const auto obj = line.substr(i, obj_end - i + 1);

        const auto px_pos = obj.find("\"price\":");
        const auto qty_pos = obj.find("\"qty\":");
        if (px_pos == std::string_view::npos || qty_pos == std::string_view::npos) {
            break;
        }
        Level level;
        level.price = std::strtod(obj.data() + px_pos + 8, nullptr);
        level.qty = static_cast<std::uint64_t>(std::strtoull(obj.data() + qty_pos + 6, nullptr, 10));
        levels.push_back(level);
        if (levels.size() >= kMaxLevelsPerSide) {
            break;
        }
        i = obj_end + 1;
    }
}

void ParseSnapshotLineInto(std::string_view line, SnapshotEvent& event) {
    if (const auto seq = JsonU64Field(line, "seq")) {
        event.seq = *seq;
    } else {
        throw std::runtime_error("jsonl: missing seq");
    }
    if (const auto ts = JsonU64Field(line, "ts_ns")) {
        event.ts_ns = *ts;
    } else {
        throw std::runtime_error("jsonl: missing ts_ns");
    }
    if (const auto symbol = JsonStringField(line, "symbol")) {
        event.symbol.assign(*symbol);
    } else {
        event.symbol.clear();
    }
    JsonLevelsInto(line, "bids", event.bids);
    JsonLevelsInto(line, "asks", event.asks);
}

}  // namespace

JsonlTape::JsonlTape(std::string path, std::string only_symbol)
    : path_(std::move(path))
    , only_symbol_(std::move(only_symbol))
    , file_(path_) {
    if (!file_) {
        throw std::runtime_error("cannot open dataset: " + path_);
    }
    line_.reserve(512);
    parse_event_.bids.reserve(kMaxLevelsPerSide);
    parse_event_.asks.reserve(kMaxLevelsPerSide);
    pending_ = ReadOne();
    if (!pending_) {
        throw std::runtime_error(only_symbol_.empty()
                                     ? ("dataset is empty: " + path_)
                                     : ("dataset has no rows for symbol: " + only_symbol_));
    }
}

std::uint64_t JsonlTape::StartTs() const {
    return pending_->ts_ns;
}

const std::string& JsonlTape::FirstSymbol() const {
    return pending_->symbol;
}

std::optional<SnapshotEvent> JsonlTape::Next() {
    if (pending_) {
        SnapshotEvent event = std::move(*pending_);
        pending_.reset();
        // Restore parse buffers after move-from pending.
        if (parse_event_.bids.capacity() < kMaxLevelsPerSide) {
            parse_event_.bids.reserve(kMaxLevelsPerSide);
        }
        if (parse_event_.asks.capacity() < kMaxLevelsPerSide) {
            parse_event_.asks.reserve(kMaxLevelsPerSide);
        }
        return event;
    }
    return ReadOne();
}

std::optional<SnapshotEvent> JsonlTape::ReadOne() {
    while (std::getline(file_, line_)) {
        if (line_.empty()) {
            continue;
        }
        ParseSnapshotLineInto(line_, parse_event_);
        if (!only_symbol_.empty() && !parse_event_.symbol.empty() &&
            parse_event_.symbol != only_symbol_) {
            continue;
        }
        SnapshotEvent event;
        event.seq = parse_event_.seq;
        event.ts_ns = parse_event_.ts_ns;
        event.symbol = parse_event_.symbol;
        // Steal level vectors (capacity goes with the event into the window).
        event.bids = std::move(parse_event_.bids);
        event.asks = std::move(parse_event_.asks);
        parse_event_.bids.clear();
        parse_event_.asks.clear();
        parse_event_.bids.reserve(kMaxLevelsPerSide);
        parse_event_.asks.reserve(kMaxLevelsPerSide);
        return event;
    }
    return std::nullopt;
}

}  // namespace dataset
