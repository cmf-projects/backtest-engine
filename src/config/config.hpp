#pragma once

#include "cmf/cmf_driver.hpp"

#include <string>

/// Settings from the venue TOML (default apps/backtest/config.toml).
struct AppConfig {
    std::string host = "127.0.0.1";
    unsigned short feed_port = 9001;
    unsigned short order_port = 9002;
    std::string dataset;
    std::string class_code = "FCEU";
    std::string sec_code;
    double tick_size = 0.0001;
};

[[nodiscard]] std::string ResolveConfigPath();

[[nodiscard]] AppConfig LoadConfig(const std::string& path);

[[nodiscard]] cmf_driver::Options MakeListenOptions(const AppConfig& cfg);
