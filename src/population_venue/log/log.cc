#include "population_venue/log/log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>

#ifndef CMF_LOG_FORMAT_JSON
#define CMF_LOG_FORMAT_JSON 0
#endif

namespace population_venue {

void InitLogging() {
    auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("population_venue", sink);
    spdlog::set_default_logger(std::move(logger));

    // Match compile-time gate so runtime filter agrees with SPDLOG_* macros.
    spdlog::set_level(static_cast<spdlog::level::level_enum>(SPDLOG_ACTIVE_LEVEL));
    spdlog::flush_on(spdlog::level::warn);

#if CMF_LOG_FORMAT_JSON
    spdlog::set_pattern(R"({"ts":"%Y-%m-%dT%H:%M:%S.%e%z","level":"%l","msg":"%v"})");
#else
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
#endif
}

}  // namespace population_venue
