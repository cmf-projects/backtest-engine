#pragma once

// Compile-time log config (IMPL 5.2–5.3):
//   -DCMF_LOG_LEVEL=trace|debug|info|warn|error  → SPDLOG_ACTIVE_LEVEL (lower levels stripped)
//   -DCMF_LOG_FORMAT_JSON=0|1                    → plain vs json pattern
// Call InitLogging() once from main.
// Macros: LOG_TRACE / LOG_DEBUG / LOG_INFO / LOG_WARN / LOG_ERROR (see transport/log.hpp).

#include "transport/log.hpp"

namespace population_venue {

void InitLogging();

}  // namespace population_venue
