#include "population_venue/app/app.hpp"
#include "population_venue/config/config.hpp"

#include <cstdlib>

int main(int argc, char** argv) {
    population_venue::App app(population_venue::LoadConfig(argc, argv));
    return app.Run() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
