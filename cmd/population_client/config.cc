#include "population_venue/log/log.hpp"
#include "config.hpp"

#include <toml++/toml.hpp>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

using transport::BusAssetRole;
using transport::BusComponent;
using transport::BusOpen;
using transport::BusSlotConfig;

namespace client {
namespace {

[[nodiscard]] std::string EnvOr(const char* key, std::string fallback) {
    if (const char* value = std::getenv(key); value != nullptr && value[0] != '\0') {
        return value;
    }
    return fallback;
}

[[nodiscard]] const char* Env(const char* key) {
    const char* value = std::getenv(key);
    if (value == nullptr || value[0] == '\0') {
        return nullptr;
    }
    return value;
}

[[nodiscard]] std::int64_t ParseInt(std::string_view text, const char* where, std::int64_t min) {
    std::int64_t value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value < min) {
        throw std::runtime_error(std::string(where) + ": integer must be >= " + std::to_string(min));
    }
    return value;
}

[[nodiscard]] std::string TomlString(const toml::table& table, const char* key, std::string fallback) {
    if (const auto* value = table[key].as_string()) {
        return value->get();
    }
    return fallback;
}

void ApplyToml(const toml::table& table, Config& cfg) {
    cfg.uds_path = TomlString(table, "uds_path", cfg.uds_path);
    cfg.dataset = TomlString(table, "dataset", cfg.dataset);
    cfg.component_id = TomlString(table, "component_id", cfg.component_id);
    cfg.class_code = TomlString(table, "class_code", cfg.class_code);
    cfg.sec_code = TomlString(table, "sec_code", cfg.sec_code);
    cfg.tick_size = TomlString(table, "tick_size", cfg.tick_size);
    cfg.impact_lambda = TomlString(table, "impact_lambda", cfg.impact_lambda);
    if (const auto* n_pops = table["n_pops"].as_integer()) {
        if (n_pops->get() < 1 || n_pops->get() > 100000) {
            throw std::runtime_error("toml: n_pops must be an integer >= 1");
        }
        cfg.n_pops = static_cast<int>(n_pops->get());
    }
    if (const auto* lag = table["md_lag_ns"].as_integer()) {
        if (lag->get() < 0) {
            throw std::runtime_error("toml: md_lag_ns must be an integer >= 0");
        }
        cfg.md_lag_ns = static_cast<std::uint64_t>(lag->get());
    }
    if (const auto* delta = table["delta_ns"].as_integer()) {
        if (delta->get() < 1) {
            throw std::runtime_error("toml: delta_ns must be an integer >= 1");
        }
        cfg.delta_ns = static_cast<std::uint64_t>(delta->get());
    }
}

void ApplyEnv(Config& cfg) {
    cfg.uds_path = EnvOr("POPULATION_UDS", cfg.uds_path);
    cfg.dataset = EnvOr("POPULATION_DATASET", cfg.dataset);
    cfg.sec_code = EnvOr("POPULATION_SEC_CODE", cfg.sec_code);
    cfg.tick_size = EnvOr("POPULATION_TICK_SIZE", cfg.tick_size);
    cfg.impact_lambda = EnvOr("POPULATION_IMPACT_LAMBDA", cfg.impact_lambda);
    if (const char* pops = Env("CLIENT_POPS")) {
        cfg.n_pops = static_cast<int>(ParseInt(pops, "CLIENT_POPS", 1));
    }
}

void ApplyArgv(Config& cfg, int argc, char** argv) {
    if (argc >= 2 && argv[1] != nullptr && argv[1][0] != '\0') {
        cfg.uds_path = argv[1];
    }
    if (argc >= 3 && argv[2] != nullptr && argv[2][0] != '\0') {
        cfg.dataset = argv[2];
    }
    if (argc >= 4 && argv[3] != nullptr && argv[3][0] != '\0') {
        cfg.n_pops = static_cast<int>(ParseInt(argv[3], "argv n_pops", 1));
    }
}

void Validate(const Config& cfg) {
    if (cfg.sec_code.empty()) {
        throw std::runtime_error("config: sec_code is empty");
    }
    if (cfg.tick_size.empty()) {
        throw std::runtime_error("config: tick_size is empty");
    }
    if (cfg.impact_lambda.empty()) {
        throw std::runtime_error("config: impact_lambda is empty");
    }
    std::size_t idx = 0;
    const double lambda = std::stod(cfg.impact_lambda, &idx);
    if (idx != cfg.impact_lambda.size() || !(lambda > 0.0)) {
        throw std::runtime_error("config: impact_lambda must be > 0");
    }
    if (cfg.delta_ns < 1) {
        throw std::runtime_error("config: delta_ns must be >= 1");
    }
}

}  // namespace

Config ParseConfig(int argc, char** argv) {
    Config cfg;
    const std::string config_path = EnvOr("CONFIG", "options/population_client.toml");
    if (std::filesystem::exists(config_path)) {
        try {
            ApplyToml(toml::parse_file(config_path), cfg);
        } catch (const toml::parse_error& ex) {
            throw std::runtime_error(config_path + ": " + std::string(ex.description()));
        }
    }

    ApplyEnv(cfg);
    ApplyArgv(cfg, argc, argv);
    Validate(cfg);

    LOG_INFO("uds={} dataset={} sec={} tick={} lambda={} pops={}", cfg.uds_path, cfg.dataset,
             cfg.sec_code, cfg.tick_size, cfg.impact_lambda, cfg.n_pops);
    return cfg;
}

BusOpen MakeOpen(const std::string& population_id, const Config& cfg) {
    BusOpen open;
    open.population_id = population_id;
    open.components.push_back(BusComponent{
        .component_id = cfg.component_id,
        .role = BusAssetRole::Spot,
        .dataset_path = cfg.dataset,
        .class_code = cfg.class_code,
        .sec_code = cfg.sec_code,
        .tick_size = cfg.tick_size,
    });
    open.slots.push_back(BusSlotConfig{
        .slot_id = 1,
        .md_lag_ns = cfg.md_lag_ns,
        .delta_ns = cfg.delta_ns,
        .impact_lambda = cfg.impact_lambda,
    });
    return open;
}

}  // namespace client
