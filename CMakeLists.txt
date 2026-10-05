cmake_minimum_required(VERSION 3.14)   # FetchContent_MakeAvailable

project(cmf
  VERSION 1.0.0
  DESCRIPTION "CMF venue driver and a worked example"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# asio, standalone
#
# Fetched rather than vendored so there is one line to change when it moves.
# The tag is pinned: a floating branch would mean two people building this at
# different times compile against different headers, which is the one thing a
# reproducible backtest cannot afford.
#
# Offline or behind a proxy? Point CMake at a copy you already have and nothing
# is downloaded:
#
#     cmake -B build -DFETCHCONTENT_SOURCE_DIR_ASIO=/path/to/asio
#
# asio's repository has no CMakeLists.txt of its own, so this only unpacks the
# sources; the target below is ours.
# ---------------------------------------------------------------------------
set(CMF_ASIO_TAG "asio-1-30-2" CACHE STRING "Git tag of the asio release to build against")

include(FetchContent)

FetchContent_Declare(asio
  GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
  GIT_TAG        ${CMF_ASIO_TAG}
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE)

FetchContent_MakeAvailable(asio)

find_package(Threads REQUIRED)

add_library(asio_standalone INTERFACE)
add_library(cmf::asio ALIAS asio_standalone)

target_include_directories(asio_standalone SYSTEM INTERFACE "${asio_SOURCE_DIR}/asio/include")
target_compile_definitions(asio_standalone INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_link_libraries(asio_standalone INTERFACE Threads::Threads)

# ---------------------------------------------------------------------------
# The driver: one header, no sources.
# ---------------------------------------------------------------------------
add_library(cmf_driver INTERFACE)
add_library(cmf::driver ALIAS cmf_driver)

target_include_directories(cmf_driver INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(cmf_driver INTERFACE cmf::asio)
target_sources(cmf_driver INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cmf_driver.hpp>")

# ---------------------------------------------------------------------------
# The worked example.
#
# It is built as `backtest` because that is the name the engine looks for, and
# it looks for it next to itself. Copy the result there and a run works; keep
# the name when you replace the example with your own venue.
# ---------------------------------------------------------------------------
add_executable(backtest example_venue.cc)
target_link_libraries(backtest PRIVATE cmf::driver)

if(MSVC)
  target_compile_options(backtest PRIVATE /W4)
else()
  target_compile_options(backtest PRIVATE -Wall -Wextra -Wpedantic)
endif()
