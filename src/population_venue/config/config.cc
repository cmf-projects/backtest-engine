#include "population_venue/config/config.hpp"

#include <toml++/toml.hpp>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace population_venue {
namespace {

[[nodiscard]] std::string EnvOr(const char* key, std::string fallback) {
    if (const char* v = std::getenv(key); v != nullptr && v[0] != '\0') {
        return v;
    }
    return fallback;
}

[[nodiscard]] const char* Env(const char* key) {
    const char* v = std::getenv(key);
    if (v == nullptr || v[0] == '\0') {
        return nullptr;
    }
    return v;
}

[[nodiscard]] std::uint32_t ParseDrain(std::string_view text, const char* where) {
    std::uint32_t value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value < 1) {
        throw std::runtime_error(std::string(where) + ": drain_timeout_ms must be an integer >= 1");
    }
    return value;
}

void ApplyToml(const toml::table& table, Config& cfg) {
    if (const auto* uds = table["uds_path"].as_string()) {
        cfg.uds_path = uds->get();
    }
    if (const auto* drain = table["drain_timeout_ms"].as_integer()) {
        const auto value = drain->get();
        if (value < 1 || value > static_cast<std::int64_t>(UINT32_MAX)) {
            throw std::runtime_error("toml: drain_timeout_ms must be an integer >= 1");
        }
        cfg.drain_timeout_ms = static_cast<std::uint32_t>(value);
    }
}

}  // namespace

Config LoadConfig(int argc, char** argv) {
    Config cfg;

    const std::string config_path = EnvOr("CONFIG", "options/population_venue.toml");
    if (std::filesystem::exists(config_path)) {
        try {
            ApplyToml(toml::parse_file(config_path), cfg);
        } catch (const toml::parse_error& ex) {
            throw std::runtime_error(config_path + ": " + std::string(ex.description()));
        }
    }

    cfg.uds_path = EnvOr("POPULATION_UDS", cfg.uds_path);
    if (const char* drain = Env("POPULATION_DRAIN_MS")) {
        cfg.drain_timeout_ms = ParseDrain(drain, "POPULATION_DRAIN_MS");
    }

    if (argc >= 2 && argv != nullptr && argv[1] != nullptr && argv[1][0] != '\0') {
        cfg.uds_path = argv[1];
    }
    return cfg;
}

}  // namespace population_venue
