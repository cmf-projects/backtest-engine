#include "population_venue/app/app.hpp"
#include "population_venue/config/config.hpp"
#include "population_venue/log/log.hpp"
#include "population_venue/shutdown/shutdown.hpp"

#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    population_venue::InstallShutdownHandlers();
    population_venue::InitLogging();
    try {
        population_venue::App app(population_venue::LoadConfig(argc, argv));
        return app.Run() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& ex) {
        std::cerr << "config: " << ex.what() << '\n';
        return EXIT_FAILURE;
    }
}
