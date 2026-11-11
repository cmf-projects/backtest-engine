#pragma once

#include "transport/control.hpp"

#include <cstdint>
#include <string>

namespace population_venue {

struct Config {
    std::string uds_path = "/tmp/cmf-population.sock";
    std::uint32_t drain_timeout_ms = transport::kDefaultDrainTimeoutMs;
};

// Default TOML: options/population_venue.toml.
// ENV: CONFIG, POPULATION_UDS, POPULATION_DRAIN_MS; optional argv[1] = uds_path.
// Log level/format are compile-time (CMake CMF_LOG_LEVEL / CMF_LOG_FORMAT).
[[nodiscard]] Config LoadConfig(int argc, char** argv);

}  // namespace population_venue
