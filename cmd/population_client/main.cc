#include "population_venue/log/log.hpp"
#include "config.hpp"
#include "run.hpp"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <exception>

int main(int argc, char** argv) {
    population_venue::InitLogging();
    try {
        const client::Config cfg = client::ParseConfig(argc, argv);
        if (!cfg.ok) {
            spdlog::shutdown();
            return EXIT_FAILURE;
        }
        const int rc = client::Run(cfg);
        spdlog::shutdown();
        return rc;
    } catch (const std::exception& ex) {
        LOG_ERROR("config: {}", ex.what());
        spdlog::shutdown();
        return EXIT_FAILURE;
    }
}
