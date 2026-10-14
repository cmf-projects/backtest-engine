#include "config/config.hpp"
#include "venue/venue.hpp"

#include "cmf/cmf_driver.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>

int main() {
    try {
        const std::string config_path = ResolveConfigPath();
        std::fprintf(stderr, "venue: config %s\n", config_path.c_str());
        const AppConfig cfg = LoadConfig(config_path);
        RunVenue(cfg);
        return EXIT_SUCCESS;

    } catch (const cmf_driver::ProtocolError& error) {
        std::fprintf(stderr, "venue: PROTOCOL ERROR: %s\n", error.what());
        return EXIT_FAILURE;

    } catch (const std::exception& error) {
        std::fprintf(stderr, "venue: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
