cmake_minimum_required(VERSION 3.20)
project(pmr_market_simulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MSIM_BUILD_TESTS "Build unit tests" ON)
option(MSIM_WARNINGS_AS_ERRORS "Treat warnings in project code as errors" ON)
option(MSIM_FUZZ "Build the libFuzzer target (needs Clang with libFuzzer)" OFF)
option(MSIM_PORTABLE_SHA256 "Use portable SHA-256 even when the system provider is available" OFF)
set(MSIM_SANITIZE "" CACHE STRING
    "Comma-separated -fsanitize list, e.g. address,undefined or thread")

# ──────────────────────────────────────────────
# Shared flags
# ──────────────────────────────────────────────

add_library(msim_flags INTERFACE)
if(MSVC)
  target_compile_options(msim_flags INTERFACE /W4 /permissive-)
  if(MSIM_WARNINGS_AS_ERRORS)
    target_compile_options(msim_flags INTERFACE /WX)
  endif()
else()
  target_compile_options(msim_flags INTERFACE
      -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
  if(MSIM_WARNINGS_AS_ERRORS)
    target_compile_options(msim_flags INTERFACE -Werror)
  endif()
endif()

if(MSIM_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "MSIM_SANITIZE is only wired up for Clang and GCC")
  endif()
  add_compile_options(-fsanitize=${MSIM_SANITIZE} -fno-omit-frame-pointer
                      -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${MSIM_SANITIZE})
endif()

find_package(Threads REQUIRED)

# ──────────────────────────────────────────────
# Core containers and command/workload helpers. Workload serialization owns
# its file I/O; matching operations do not depend on that path.
# ──────────────────────────────────────────────

add_library(marketsim_core
    src/order_book.cpp
    src/feed_book.cpp
    src/book_types.cpp
    src/command.cpp
    src/workload.cpp
    src/pmr_utils.cpp
)
target_include_directories(marketsim_core PUBLIC include)
target_link_libraries(marketsim_core PRIVATE msim_flags)
if(MSIM_PORTABLE_SHA256)
  # Public because InputFile contains the selected provider's value type.
  target_compile_definitions(marketsim_core PUBLIC MSIM_PORTABLE_SHA256=1)
endif()
if(APPLE AND NOT MSIM_PORTABLE_SHA256)
  message(STATUS "Input SHA-256 provider: CommonCrypto (system)")
else()
  message(STATUS "Input SHA-256 provider: portable")
endif()

# The reference book and the differential harness that checks the engine
# against it. Kept out of core so nothing on the matching path depends on it.
add_library(marketsim_verify
    src/reference_book.cpp
    src/differential.cpp
)
target_link_libraries(marketsim_verify PUBLIC marketsim_core PRIVATE msim_flags)

# Matching benchmarks link the independent verifier for untimed semantic
# checks; the timer encloses only the selected command loop.
if(CMAKE_BUILD_TYPE)
  string(TOUPPER "${CMAKE_BUILD_TYPE}" msim_bt)
  set(MSIM_EFFECTIVE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${msim_bt}}")
else()
  set(MSIM_EFFECTIVE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()
string(STRIP "${MSIM_EFFECTIVE_CXX_FLAGS}" MSIM_EFFECTIVE_CXX_FLAGS)
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported AND NOT MSIM_SANITIZE)
  set(MSIM_IPO "on for Release")
else()
  set(MSIM_IPO "off")
endif()
configure_file(bench/build_info.hpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.hpp @ONLY)
add_executable(book_bench bench/book_bench.cpp)
target_include_directories(book_bench PRIVATE
                           ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(book_bench PRIVATE marketsim_verify msim_flags)

add_executable(structure_bench bench/structure_bench.cpp)
target_link_libraries(structure_bench PRIVATE marketsim_core msim_flags)

add_executable(book_demo tools/book_demo.cpp)
target_link_libraries(book_demo PRIVATE marketsim_core msim_flags)

add_executable(diff_replay tools/diff_replay.cpp)
target_link_libraries(diff_replay PRIVATE marketsim_verify msim_flags)

add_library(marketsim_replay src/replay.cpp)
target_include_directories(marketsim_replay PUBLIC include)
target_link_libraries(marketsim_replay PUBLIC marketsim_core Threads::Threads PRIVATE msim_flags)

add_executable(replay_bench bench/replay_bench.cpp)
target_link_libraries(replay_bench PRIVATE marketsim_replay msim_flags)

add_executable(itch_replay tools/itch_replay.cpp)
target_link_libraries(itch_replay PRIVATE marketsim_replay msim_flags
                      Threads::Threads)

add_executable(itch_volume tools/itch_volume.cpp)
target_link_libraries(itch_volume PRIVATE marketsim_core msim_flags)

add_executable(itch_observe tools/itch_observe.cpp)
target_link_libraries(itch_observe PRIVATE marketsim_core msim_flags)

if(MSIM_FUZZ)
  add_executable(differential_fuzz fuzz/differential_fuzz.cpp)
  target_compile_options(differential_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(differential_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_libraries(differential_fuzz PRIVATE marketsim_verify msim_flags)
endif()

if(MSIM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ──────────────────────────────────────────────
# Link-time optimization for the optimized builds
# ──────────────────────────────────────────────

if(ipo_supported AND NOT MSIM_SANITIZE)
  foreach(t marketsim_core marketsim_verify marketsim_replay book_bench structure_bench replay_bench itch_replay itch_volume itch_observe)
    set_property(TARGET ${t} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  endforeach()
endif()
