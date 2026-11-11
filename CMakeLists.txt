cmake_minimum_required(VERSION 3.14)   # FetchContent_MakeAvailable

project(cmf
  VERSION 1.0.0
  DESCRIPTION "Population venue (UDS + SPSC) and core"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

include(FetchContent)

# ---------------------------------------------------------------------------
# toml++
# ---------------------------------------------------------------------------
set(CMF_TOMLPP_TAG "v3.4.0" CACHE STRING "Git tag of toml++")

FetchContent_Declare(tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG        ${CMF_TOMLPP_TAG}
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE)

FetchContent_MakeAvailable(tomlplusplus)

# ---------------------------------------------------------------------------
# spdlog (venue logging; IMPL 5.2–5.3) — level/format baked at compile time
# ---------------------------------------------------------------------------
set(CMF_LOG_LEVEL "info" CACHE STRING "Compile-time log level: trace|debug|info|warn|error")
set_property(CACHE CMF_LOG_LEVEL PROPERTY STRINGS trace debug info warn error)
set(CMF_LOG_FORMAT "plain" CACHE STRING "Compile-time log format: plain|json")
set_property(CACHE CMF_LOG_FORMAT PROPERTY STRINGS plain json)

if(CMF_LOG_LEVEL STREQUAL "trace")
  set(CMF_SPDLOG_ACTIVE_LEVEL "SPDLOG_LEVEL_TRACE")
elseif(CMF_LOG_LEVEL STREQUAL "debug")
  set(CMF_SPDLOG_ACTIVE_LEVEL "SPDLOG_LEVEL_DEBUG")
elseif(CMF_LOG_LEVEL STREQUAL "warn")
  set(CMF_SPDLOG_ACTIVE_LEVEL "SPDLOG_LEVEL_WARN")
elseif(CMF_LOG_LEVEL STREQUAL "error")
  set(CMF_SPDLOG_ACTIVE_LEVEL "SPDLOG_LEVEL_ERROR")
else()
  set(CMF_SPDLOG_ACTIVE_LEVEL "SPDLOG_LEVEL_INFO")
endif()

if(CMF_LOG_FORMAT STREQUAL "json")
  set(CMF_LOG_FORMAT_JSON 1)
else()
  set(CMF_LOG_FORMAT_JSON 0)
endif()

set(CMF_SPDLOG_TAG "v1.15.1" CACHE STRING "Git tag of spdlog")
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)

FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        ${CMF_SPDLOG_TAG}
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE)

FetchContent_MakeAvailable(spdlog)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Dataset + population core — src/
# ---------------------------------------------------------------------------
add_library(dataset_lib
  src/dataset/jsonl_tape.cc)
add_library(cmf::dataset ALIAS dataset_lib)

target_include_directories(dataset_lib
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
target_compile_features(dataset_lib PUBLIC cxx_std_20)

add_library(population_lib
  src/population/clock.cc
  src/population/file_book.cc
  src/population/tape_window.cc
  src/population/merged_tape.cc
  src/population/book_diff.cc
  src/population/slot.cc
  src/population/slot_orders.cc
  src/population/match.cc
  src/population/impact.cc
  src/population/population.cc)
add_library(cmf::population ALIAS population_lib)

target_include_directories(population_lib
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
target_compile_features(population_lib PUBLIC cxx_std_20)
# dataset_lib adapts into population::Tape (headers only); link both at the app.

# ---------------------------------------------------------------------------
# Transport — src/transport/ (UDS control + SPSC ring header)
# ---------------------------------------------------------------------------
add_library(transport_lib
  src/transport/shm_queue.cc
  src/transport/uds_framing.cc
  src/transport/uds_control.cc
  src/transport/bus_framing.cc
  src/transport/bus_population_map.cc
  src/transport/spsc_bus.cc
  src/transport/population_session.cc)
add_library(cmf::transport ALIAS transport_lib)

target_include_directories(transport_lib
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
target_compile_features(transport_lib PUBLIC cxx_std_20)
target_link_libraries(transport_lib PUBLIC cmf::population cmf::dataset spdlog::spdlog Threads::Threads)
target_compile_definitions(transport_lib
  PUBLIC
    SPDLOG_ACTIVE_LEVEL=${CMF_SPDLOG_ACTIVE_LEVEL})
# shm_open lives in librt on older glibc; no-op / unused on modern libcs that fold it in.
find_library(CMF_LIBRT rt)
if(CMF_LIBRT)
  target_link_libraries(transport_lib PUBLIC ${CMF_LIBRT})
endif()

# ---------------------------------------------------------------------------
# Population venue app
# ---------------------------------------------------------------------------
add_library(population_venue_lib
  src/population_venue/config/config.cc
  src/population_venue/shutdown/shutdown.cc
  src/population_venue/log/log.cc
  src/population_venue/app/app.cc)
add_library(cmf::population_venue ALIAS population_venue_lib)
target_include_directories(population_venue_lib
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
target_compile_features(population_venue_lib PUBLIC cxx_std_20)
target_link_libraries(population_venue_lib
  PUBLIC cmf::transport spdlog::spdlog
  PRIVATE tomlplusplus::tomlplusplus)
target_compile_definitions(population_venue_lib
  PUBLIC
    SPDLOG_ACTIVE_LEVEL=${CMF_SPDLOG_ACTIVE_LEVEL}
    CMF_LOG_FORMAT_JSON=${CMF_LOG_FORMAT_JSON})

add_executable(population_venue apps/population_venue/main.cc)
target_link_libraries(population_venue PRIVATE cmf::population_venue)

# Same-process smoke: UDS create + shared QueueRegistry tick loop.
add_executable(population_smoke apps/population_smoke/main.cc)
target_link_libraries(population_smoke PRIVATE cmf::transport)

add_executable(population_client apps/population_client/main.cc)
target_link_libraries(population_client PRIVATE cmf::transport)

# ---------------------------------------------------------------------------
# Tests (GoogleTest) — population + transport
# ---------------------------------------------------------------------------
option(CMF_BUILD_TESTS "Build unit tests" ON)

if(CMF_BUILD_TESTS)
  set(CMF_GTEST_TAG "v1.15.2" CACHE STRING "GoogleTest git tag")
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        ${CMF_GTEST_TAG}
    GIT_SHALLOW    TRUE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  include(GoogleTest)

  add_executable(population_tests
    tests/population/population_tick_test.cc
    tests/population/population_bundle_test.cc
    tests/population/tape_window_test.cc
    tests/population/merged_tape_test.cc
    tests/population/order_submit_test.cc
    tests/population/match_test.cc
    tests/population/impact_test.cc)
  target_include_directories(population_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/population")
  target_link_libraries(population_tests PRIVATE
    cmf::population
    GTest::gtest_main)
  gtest_discover_tests(population_tests)

  add_executable(transport_tests
    tests/transport/spsc_ring_test.cc
    tests/transport/shm_queue_test.cc
    tests/transport/control_test.cc
    tests/transport/queue_registry_test.cc
    tests/transport/uds_control_test.cc
    tests/transport/finish_session_test.cc
    tests/transport/spsc_bus_test.cc
    tests/transport/bus_framing_test.cc
    tests/transport/population_session_test.cc)
  target_include_directories(transport_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/population")
  target_link_libraries(transport_tests PRIVATE
    cmf::transport
    GTest::gtest_main)
  gtest_discover_tests(transport_tests)
endif()

if(MSVC)
  target_compile_options(dataset_lib PRIVATE /W4)
  target_compile_options(population_lib PRIVATE /W4)
  target_compile_options(transport_lib PRIVATE /W4)
  target_compile_options(population_venue_lib PRIVATE /W4)
  target_compile_options(population_venue PRIVATE /W4)
  target_compile_options(population_smoke PRIVATE /W4)
  target_compile_options(population_client PRIVATE /W4)
  if(CMF_BUILD_TESTS)
    target_compile_options(population_tests PRIVATE /W4)
    target_compile_options(transport_tests PRIVATE /W4)
  endif()
else()
  target_compile_options(dataset_lib PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(population_lib PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(transport_lib PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(population_venue_lib PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(population_venue PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(population_smoke PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(population_client PRIVATE -Wall -Wextra -Wpedantic)
  if(CMF_BUILD_TESTS)
    target_compile_options(population_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(transport_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()
