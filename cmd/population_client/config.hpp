#pragma once

#include "transport/bus_messages.hpp"

#include <cstdint>
#include <string>

namespace client {

struct Config {
    std::string uds_path = "/tmp/cmf-population.sock";
    std::string dataset = "testdata/population/smoke.jsonl";
    int n_pops = 1;

    std::string component_id = "SPOT";
    std::string class_code = "TQBR";
    std::string sec_code = "SBER";
    std::string tick_size = "0.01";
    std::uint64_t md_lag_ns = 0;
    std::uint64_t delta_ns = 1;
    std::string impact_lambda = "1e-15";

    bool ok = true;
};

// Defaults, then options/population_client.toml, then env, then argv.
[[nodiscard]] Config ParseConfig(int argc, char** argv);

[[nodiscard]] transport::BusOpen MakeOpen(const std::string& population_id, const Config& cfg);

}  // namespace client
