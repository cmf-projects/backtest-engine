# Local workflow: population UDS venue + smoke.
#
#   make build             configure + compile
#   make population         run population UDS venue
#   make population-smoke   same-process Open→Tick→Advance→End (shm attach)
#   make population-client  external Engine: take best bid/ask
#   make clean
#   make test               build + ctest

BUILD_DIR        ?= build
POPULATION_VENUE := $(BUILD_DIR)/population_venue
POPULATION_SMOKE := $(BUILD_DIR)/population_smoke
POPULATION_CLIENT := $(BUILD_DIR)/population_client
POPULATION_CONFIG ?= options/population_venue.toml
POPULATION_UDS   ?= /tmp/cmf-population.sock
POPULATION_DATASET ?= testdata/population/smoke.jsonl
CMF_LOG_LEVEL    ?= info
CMF_LOG_FORMAT   ?= plain
CMAKE_FLAGS      ?= -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
                    -DCMF_LOG_LEVEL=$(CMF_LOG_LEVEL) -DCMF_LOG_FORMAT=$(CMF_LOG_FORMAT)

.PHONY: all build cmake population population-smoke population-client clean help test

all: build

help:
	@echo "Targets:"
	@echo "  make build              - cmake + compile"
	@echo "  make population         - population UDS venue (CONFIG=$(POPULATION_CONFIG))"
	@echo "  make population-smoke   - Open/Tick/Advance/End smoke (shm SPSC)"
	@echo "  make population-client  - external take bid/ask client → $(POPULATION_UDS)"
	@echo "  make test               - build + ctest"
	@echo "  make clean              - remove $(BUILD_DIR)"
	@echo "Log (compile-time): CMF_LOG_LEVEL=$(CMF_LOG_LEVEL) CMF_LOG_FORMAT=$(CMF_LOG_FORMAT)"
	@echo "  e.g. make clean && make build CMF_LOG_LEVEL=debug CMF_LOG_FORMAT=json"

cmake: $(BUILD_DIR)/CMakeCache.txt

$(BUILD_DIR)/CMakeCache.txt:
	cmake -B $(BUILD_DIR) $(CMAKE_FLAGS)

build: cmake
	cmake --build $(BUILD_DIR) -j
	@# Convenience for clangd when compile_commands lives in build/
	@if [ -f $(BUILD_DIR)/compile_commands.json ] && [ ! -e compile_commands.json ]; then \
		ln -sf $(BUILD_DIR)/compile_commands.json compile_commands.json; \
	fi

$(POPULATION_VENUE): build
$(POPULATION_SMOKE): build
$(POPULATION_CLIENT): build

population: $(POPULATION_VENUE)
	CONFIG=$(POPULATION_CONFIG) POPULATION_UDS=$(POPULATION_UDS) ./$(POPULATION_VENUE)

population-smoke: $(POPULATION_SMOKE)
	./$(POPULATION_SMOKE) $(POPULATION_DATASET) $(POPULATION_UDS)-smoke

population-client: $(POPULATION_CLIENT)
	./$(POPULATION_CLIENT) $(POPULATION_UDS) $(POPULATION_DATASET)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

clean:
	rm -rf $(BUILD_DIR)
