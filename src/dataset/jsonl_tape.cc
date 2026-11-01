#include "dataset/jsonl_tape.hpp"

#include <charconv>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace dataset {
namespace {

using population::kMaxLevelsPerSide;
using population::Level;
using population::SnapshotEvent;

[[nodiscard]] std::optional<std::string_view> JsonStringField(std::string_view line,
                                                              std::string_view key) {
    const std::string pattern = "\"" + std::string(key) + "\":\"";
    const auto pos = line.find(pattern);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    const auto start = pos + pattern.size();
    const auto end = line.find('"', start);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    return line.substr(start, end - start);
}

[[nodiscard]] std::optional<std::uint64_t> JsonU64Field(std::string_view line,
                                                        std::string_view key) {
    const std::string pattern = "\"" + std::string(key) + "\":";
    const auto pos = line.find(pattern);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    const auto start = pos + pattern.size();
    std::uint64_t value = 0;
    const auto* begin = line.data() + start;
    const auto* end = line.data() + line.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr == begin) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::vector<Level> JsonLevels(std::string_view line, std::string_view key) {
    const std::string pattern = "\"" + std::string(key) + "\":[";
    const auto pos = line.find(pattern);
    if (pos == std::string_view::npos) {
        return {};
    }
    auto i = pos + pattern.size();
    std::vector<Level> levels;
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

        Level level;
        const auto px_pos = obj.find("\"price\":");
        const auto qty_pos = obj.find("\"qty\":");
        if (px_pos == std::string_view::npos || qty_pos == std::string_view::npos) {
            break;
        }
        level.price = std::strtod(obj.data() + px_pos + 8, nullptr);
        level.qty = static_cast<std::uint64_t>(std::strtoull(obj.data() + qty_pos + 6, nullptr, 10));
        levels.push_back(level);
        i = obj_end + 1;
    }
    return levels;
}

[[nodiscard]] SnapshotEvent ParseSnapshotLine(std::string_view line) {
    SnapshotEvent event;
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
        event.symbol = std::string(*symbol);
    }
    event.bids = JsonLevels(line, "bids");
    event.asks = JsonLevels(line, "asks");
    if (event.bids.size() > kMaxLevelsPerSide) {
        event.bids.resize(kMaxLevelsPerSide);
    }
    if (event.asks.size() > kMaxLevelsPerSide) {
        event.asks.resize(kMaxLevelsPerSide);
    }
    return event;
}

}  // namespace

JsonlTape::JsonlTape(std::string path, std::string only_symbol)
    : path_(std::move(path))
    , only_symbol_(std::move(only_symbol))
    , file_(path_) {
    if (!file_) {
        throw std::runtime_error("cannot open dataset: " + path_);
    }
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
        return event;
    }
    return ReadOne();
}

std::optional<SnapshotEvent> JsonlTape::ReadOne() {
    std::string line;
    while (std::getline(file_, line)) {
        if (line.empty()) {
            continue;
        }
        SnapshotEvent event = ParseSnapshotLine(line);
        if (!only_symbol_.empty() && !event.symbol.empty() && event.symbol != only_symbol_) {
            continue;
        }
        return event;
    }
    return std::nullopt;
}

}  // namespace dataset
