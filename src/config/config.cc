#include "config/config.hpp"

#include <toml++/toml.hpp>

#include <cstdlib>
#include <stdexcept>

std::string ResolveConfigPath() {
    if (const char* env = std::getenv("CONFIG"); env && *env) {
        return env;
    }
    return "apps/backtest/config.toml";
}

AppConfig LoadConfig(const std::string& path) {
    const toml::table table = toml::parse_file(path);
    AppConfig cfg;

    if (const auto* v = table["host"].as_string()) {
        cfg.host = v->get();
    }
    if (const auto* v = table["feed_port"].as_integer()) {
        cfg.feed_port = static_cast<unsigned short>(v->get());
    }
    if (const auto* v = table["order_port"].as_integer()) {
        cfg.order_port = static_cast<unsigned short>(v->get());
    }
    if (const auto* v = table["dataset"].as_string()) {
        cfg.dataset = v->get();
    }
    if (const auto* v = table["class_code"].as_string()) {
        cfg.class_code = v->get();
    }
    if (const auto* v = table["sec_code"].as_string()) {
        cfg.sec_code = v->get();
    }
    if (const auto* v = table["tick_size"].as_floating_point()) {
        cfg.tick_size = v->get();
    } else if (const auto* v = table["tick_size"].as_integer()) {
        cfg.tick_size = static_cast<double>(v->get());
    }

    if (cfg.dataset.empty()) {
        throw std::runtime_error(path + ": dataset is required");
    }
    if (cfg.tick_size <= 0.0) {
        throw std::runtime_error(path + ": tick_size must be positive");
    }
    if (cfg.class_code.empty() || cfg.class_code.size() > 7) {
        throw std::runtime_error(path + ": class_code must be 1..7 chars");
    }
    if (cfg.feed_port == 0 || cfg.order_port == 0) {
        throw std::runtime_error(path + ": feed_port / order_port must be non-zero");
    }
    return cfg;
}

cmf_driver::Options MakeListenOptions(const AppConfig& cfg) {
    cmf_driver::Options options;
    options.host = cfg.host;
    options.feed_port = cfg.feed_port;
    options.order_port = cfg.order_port;
    return options;
}
