#pragma once

// Compile-time log config (IMPL 5.2–5.3):
//   -DCMF_LOG_LEVEL=trace|debug|info|warn|error  → SPDLOG_ACTIVE_LEVEL (lower levels stripped)
//   -DCMF_LOG_FORMAT_JSON=0|1                    → plain vs json pattern
// Call InitLogging() once from main.

#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO
#endif

#include <spdlog/spdlog.h>

namespace population_venue {

void InitLogging();

}  // namespace population_venue

// Compile-time gated (below SPDLOG_ACTIVE_LEVEL → no code in the binary).
#define PV_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define PV_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define PV_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define PV_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define PV_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
