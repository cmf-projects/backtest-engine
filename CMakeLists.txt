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

# ---------------------------------------------------------------------------
# App entrypoint — apps/backtest → binary name `backtest` (engine looks for it)
# ---------------------------------------------------------------------------
add_executable(backtest apps/backtest/main.cc)
target_link_libraries(backtest PRIVATE cmf::venue)

if(MSVC)
  target_compile_options(venue_lib PRIVATE /W4)
  target_compile_options(backtest PRIVATE /W4)
else()
  target_compile_options(venue_lib PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(backtest PRIVATE -Wall -Wextra -Wpedantic)
endif()
