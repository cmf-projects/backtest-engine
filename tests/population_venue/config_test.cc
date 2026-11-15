#include "population_venue/config/config.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

using population_venue::LoadConfig;

namespace {

class EnvGuard {
public:
    EnvGuard(const char* key, const char* value) : key_(key) {
        if (const char* current = std::getenv(key); current != nullptr) {
            had_ = true;
            old_ = current;
        }
        if (value != nullptr) {
            if (::setenv(key, value, 1) != 0) {
                throw std::runtime_error("setenv failed");
            }
        } else if (::unsetenv(key) != 0) {
            throw std::runtime_error("unsetenv failed");
        }
    }

    ~EnvGuard() {
        if (had_) {
            ::setenv(key_.c_str(), old_.c_str(), 1);
        } else {
            ::unsetenv(key_.c_str());
        }
    }

    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    std::string key_;
    std::string old_;
    bool had_ = false;
};

std::string WriteTemp(const std::string& body) {
    static int seq = 0;
    const std::string path = "/tmp/cmf-venue-config-" + std::to_string(::getpid()) + "-" +
                             std::to_string(++seq) + ".toml";
    std::ofstream out(path);
    EXPECT_TRUE(out.good());
    out << body;
    return path;
}

}  // namespace

TEST(LoadConfig, TomlOverridesDefaults) {
    const std::string path = WriteTemp("uds_path = \"/from/toml\"\ndrain_timeout_ms = 7777\n");
    const EnvGuard config("CONFIG", path.c_str());
    const EnvGuard uds("POPULATION_UDS", nullptr);
    const EnvGuard drain("POPULATION_DRAIN_MS", nullptr);

    const auto cfg = LoadConfig(0, nullptr);
    EXPECT_EQ(cfg.uds_path, "/from/toml");
    EXPECT_EQ(cfg.drain_timeout_ms, 7777u);
}

TEST(LoadConfig, EnvOverridesToml) {
    const std::string path = WriteTemp("uds_path = \"/from/toml\"\ndrain_timeout_ms = 60000\n");
    const EnvGuard config("CONFIG", path.c_str());
    const EnvGuard uds("POPULATION_UDS", "/from/env");
    const EnvGuard drain("POPULATION_DRAIN_MS", "1234");

    const auto cfg = LoadConfig(0, nullptr);
    EXPECT_EQ(cfg.uds_path, "/from/env");
    EXPECT_EQ(cfg.drain_timeout_ms, 1234u);
}

TEST(LoadConfig, ArgvOverridesUds) {
    const std::string path = WriteTemp("uds_path = \"/from/toml\"\n");
    const EnvGuard config("CONFIG", path.c_str());
    const EnvGuard uds("POPULATION_UDS", "/from/env");
    const EnvGuard drain("POPULATION_DRAIN_MS", nullptr);
    char arg0[] = "population_venue";
    char arg1[] = "/from/argv";
    char* argv[] = {arg0, arg1, nullptr};

    const auto cfg = LoadConfig(2, argv);
    EXPECT_EQ(cfg.uds_path, "/from/argv");
}

TEST(LoadConfig, RejectsBadDrain) {
    const EnvGuard config("CONFIG", "/tmp/cmf-venue-config-missing.toml");
    const EnvGuard uds("POPULATION_UDS", nullptr);
    const EnvGuard abc("POPULATION_DRAIN_MS", "abc");
    EXPECT_THROW((void)LoadConfig(0, nullptr), std::runtime_error);
}

TEST(LoadConfig, RejectsZeroDrain) {
    const EnvGuard config("CONFIG", "/tmp/cmf-venue-config-missing.toml");
    const EnvGuard uds("POPULATION_UDS", nullptr);
    const EnvGuard zero("POPULATION_DRAIN_MS", "0");
    EXPECT_THROW((void)LoadConfig(0, nullptr), std::runtime_error);
}

TEST(LoadConfig, RejectsBrokenToml) {
    const std::string path = WriteTemp("drain_timeout_ms = [\n");
    const EnvGuard config("CONFIG", path.c_str());
    const EnvGuard uds("POPULATION_UDS", nullptr);
    const EnvGuard drain("POPULATION_DRAIN_MS", nullptr);
    EXPECT_THROW((void)LoadConfig(0, nullptr), std::runtime_error);
}

TEST(LoadConfig, MissingTomlKeepsDefaults) {
    const EnvGuard config("CONFIG", "/tmp/cmf-venue-config-missing.toml");
    const EnvGuard uds("POPULATION_UDS", nullptr);
    const EnvGuard drain("POPULATION_DRAIN_MS", nullptr);

    const auto cfg = LoadConfig(0, nullptr);
    EXPECT_EQ(cfg.uds_path, "/tmp/cmf-population.sock");
    EXPECT_EQ(cfg.drain_timeout_ms, transport::kDefaultDrainTimeoutMs);
}
