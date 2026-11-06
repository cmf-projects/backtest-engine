cmake_minimum_required(VERSION 3.14)   # FetchContent_MakeAvailable

project(cmf
  VERSION 1.0.0
  DESCRIPTION "CMF venue driver and backtest app"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# asio, standalone
# ---------------------------------------------------------------------------
set(CMF_ASIO_TAG "asio-1-30-2" CACHE STRING "Git tag of the asio release to build against")

include(FetchContent)

FetchContent_Declare(asio
  GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
  GIT_TAG        ${CMF_ASIO_TAG}
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE)

FetchContent_MakeAvailable(asio)

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

find_package(Threads REQUIRED)

add_library(asio_standalone INTERFACE)
add_library(cmf::asio ALIAS asio_standalone)

target_include_directories(asio_standalone SYSTEM INTERFACE "${asio_SOURCE_DIR}/asio/include")
target_compile_definitions(asio_standalone INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_link_libraries(asio_standalone INTERFACE Threads::Threads)

# ---------------------------------------------------------------------------
# Public protocol driver (header-only) — include/cmf/
# ---------------------------------------------------------------------------
add_library(cmf_driver INTERFACE)
add_library(cmf::driver ALIAS cmf_driver)

target_include_directories(cmf_driver INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(cmf_driver INTERFACE cmf::asio)
target_sources(cmf_driver INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/cmf/cmf_driver.hpp>")

# ---------------------------------------------------------------------------
# Venue library (private sources) — src/
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

add_library(venue_lib
  src/config/config.cc
  src/venue/venue.cc)
add_library(cmf::venue ALIAS venue_lib)

target_include_directories(venue_lib
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
  PRIVATE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(venue_lib PUBLIC cmf::driver PRIVATE tomlplusplus::tomlplusplus)

# Population path is separate from CMF venue for now; link when the UDS app arrives.
# target_link_libraries(venue_lib PUBLIC cmf::population)

# ---------------------------------------------------------------------------
# Transport — src/transport/ (UDS control + SPSC ring header)
# ---------------------------------------------------------------------------
add_library(transport_lib
  src/transport/uds_framing.cc
  src/transport/uds_control.cc)
add_library(cmf::transport ALIAS transport_lib)

target_include_directories(transport_lib
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
target_compile_features(transport_lib PUBLIC cxx_std_20)

# ---------------------------------------------------------------------------
# App entrypoint — apps/backtest → binary name `backtest` (engine looks for it)
# ---------------------------------------------------------------------------
add_executable(backtest apps/backtest/main.cc)
target_link_libraries(backtest PRIVATE cmf::venue)

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
    tests/population/tape_window_test.cc
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
    tests/transport/control_test.cc
    tests/transport/queue_registry_test.cc
    tests/transport/uds_control_test.cc)
  target_link_libraries(transport_tests PRIVATE
    cmf::transport
    GTest::gtest_main)
  gtest_discover_tests(transport_tests)
endif()

if(MSVC)
  target_compile_options(dataset_lib PRIVATE /W4)
  target_compile_options(population_lib PRIVATE /W4)
  target_compile_options(transport_lib PRIVATE /W4)
  target_compile_options(venue_lib PRIVATE /W4)
  target_compile_options(backtest PRIVATE /W4)
  if(CMF_BUILD_TESTS)
    target_compile_options(population_tests PRIVATE /W4)
    target_compile_options(transport_tests PRIVATE /W4)
  endif()
else()
  target_compile_options(dataset_lib PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(population_lib PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(transport_lib PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(venue_lib PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(backtest PRIVATE -Wall -Wextra -Wpedantic)
  if(CMF_BUILD_TESTS)
    target_compile_options(population_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(transport_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()
