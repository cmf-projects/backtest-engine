# Local workflow: C++ venue (backtest) + Python strategy stand-in.
#
#   make build          configure + compile
#   make venue          run C++ backtest only
#   make strategy       run Python strategy only (venue must already listen)
#   make run            venue in background, then strategy
#   make clean
#   make protos         generate C++ from protos/*.proto -> protos/gen/
#   make test           build + ctest (population unit smoke)

BUILD_DIR        ?= build
BACKTEST         := $(BUILD_DIR)/backtest
VENUE_CONFIG     ?= apps/backtest/config.toml
STRATEGY_CONFIG  ?= scripts/strategy.toml
STRATEGY         := python3 scripts/simple_strategy.py --config $(STRATEGY_CONFIG)
CMAKE_FLAGS      ?= -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

.PHONY: all build cmake venue strategy run clean help protos test

all: build

help:
	@echo "Targets:"
	@echo "  make build      - cmake + compile -> $(BACKTEST)"
	@echo "  make venue      - run C++ backtest (CONFIG=$(VENUE_CONFIG))"
	@echo "  make strategy   - run Python strategy (--config $(STRATEGY_CONFIG))"
	@echo "  make run        - start venue, then strategy, wait for both"
	@echo "  make protos     - generate C++ stubs into protos/gen/"
	@echo "  make test       - build + ctest (population_tests)"
	@echo "  make clean      - remove $(BUILD_DIR)"

protos:
	./protos/generate.sh

cmake: $(BUILD_DIR)/CMakeCache.txt

$(BUILD_DIR)/CMakeCache.txt:
	cmake -B $(BUILD_DIR) $(CMAKE_FLAGS)

build: cmake
	cmake --build $(BUILD_DIR) -j
	@# Convenience for clangd when compile_commands lives in build/
	@if [ -f $(BUILD_DIR)/compile_commands.json ] && [ ! -e compile_commands.json ]; then \
		ln -sf $(BUILD_DIR)/compile_commands.json compile_commands.json; \
	fi

$(BACKTEST): build

venue: $(BACKTEST)
	CONFIG=$(VENUE_CONFIG) ./$(BACKTEST)

strategy:
	$(STRATEGY)

# Venue listens; strategy dials in. Ctrl-C kills both via the shell job.
run: $(BACKTEST)
	@echo "==> starting venue ($(VENUE_CONFIG))"
	@CONFIG=$(VENUE_CONFIG) ./$(BACKTEST) & venue_pid=$$!; \
	trap 'kill $$venue_pid 2>/dev/null; wait $$venue_pid 2>/dev/null' EXIT INT TERM; \
	sleep 0.2; \
	echo "==> starting strategy ($(STRATEGY_CONFIG))"; \
	$(STRATEGY); \
	status=$$?; \
	wait $$venue_pid; \
	exit $$status

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

clean:
	rm -rf $(BUILD_DIR)
