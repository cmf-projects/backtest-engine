#include "population_venue/config/config.hpp"

#include <toml++/toml.hpp>

#include <cstdlib>

namespace population_venue {
namespace {

[[nodiscard]] std::string EnvOr(const char* key, std::string fallback) {
    if (const char* v = std::getenv(key); v != nullptr && v[0] != '\0') {
        return v;
    }
    return fallback;
}

}  // namespace

Config LoadConfig(int argc, char** argv) {
    Config cfg;
    cfg.uds_path = EnvOr("POPULATION_UDS", cfg.uds_path);
    if (const char* drain = std::getenv("POPULATION_DRAIN_MS"); drain != nullptr && drain[0] != '\0') {
        cfg.drain_timeout_ms = static_cast<std::uint32_t>(std::strtoul(drain, nullptr, 10));
    }

    const std::string config_path = EnvOr("CONFIG", "apps/population_venue/config.toml");
    try {
        const toml::table table = toml::parse_file(config_path);
        if (const auto* uds = table["uds_path"].as_string()) {
            cfg.uds_path = uds->get();
        }
        if (const auto* drain = table["drain_timeout_ms"].as_integer()) {
            cfg.drain_timeout_ms = static_cast<std::uint32_t>(drain->get());
        }
        cfg.uds_path = EnvOr("POPULATION_UDS", cfg.uds_path);
    } catch (const toml::parse_error&) {
        // Optional file; env/defaults still apply.
    }

    if (argc >= 2 && argv[1] != nullptr && argv[1][0] != '\0') {
        cfg.uds_path = argv[1];
    }
    return cfg;
}

}  // namespace population_venue
