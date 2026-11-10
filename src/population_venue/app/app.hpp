#pragma once

#include "population_venue/config/config.hpp"

namespace population_venue {

// Listen UDS; each CreateQueues starts a PopulationSession on its own thread
// (many pops concurrent on one control socket).
class App {
public:
    explicit App(Config cfg);

    // Blocks until accept/control loop fails. Returns process exit code.
    [[nodiscard]] int Run();

private:
    Config cfg_;
};

}  // namespace population_venue
