#pragma once

#include "population/tape.hpp"

#include <string>

namespace dataset {

// Picks the adapter by dataset_path:
//   *.jsonl                → JsonlTape (symbol optional filter)
//   *.feather or directory → FeatherBboTape (symbol = Databento raw_symbol, required)
[[nodiscard]] population::TapePtr MakeTape(const std::string& path, const std::string& symbol);

}  // namespace dataset
