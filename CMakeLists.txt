cmake_minimum_required(VERSION 3.15)
project(pmr_market_simulator LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ──────────────────────────────────────────────
# Core marketsim library (PMR, order book, LMDB)
# ──────────────────────────────────────────────

add_library(marketsim
    src/order_book.cpp
    src/simulator.cpp
    src/storage.cpp
    src/pmr_utils.cpp
    src/lmdb_storage.cpp
    src/lmdb_reader.cpp
    lmdb/mdb.c   # LMDB core engine
    lmdb/midl.c  # LMDB ID list helper
)

target_include_directories(marketsim
  PUBLIC include
  PUBLIC lmdb
)

# Enable Interprocedural Optimization (LTO) if available
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported)
  set_property(TARGET marketsim PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# ──────────────────────────────────────────────
# Build the market_sim executable
# ──────────────────────────────────────────────
add_executable(market_sim src/main.cpp)
target_link_libraries(market_sim PRIVATE marketsim)

if(ipo_supported)
  set_property(TARGET market_sim PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# ──────────────────────────────────────────────
# Optional stats flag
# ──────────────────────────────────────────────
option(ENABLE_STATS "Enable extra stats logging" ON)
if(ENABLE_STATS)
  target_compile_definitions(marketsim PUBLIC ENABLE_STATS=1)
  target_compile_definitions(market_sim PUBLIC ENABLE_STATS=1)
endif()

# ──────────────────────────────────────────────
# Windows-specific cleanups
# ──────────────────────────────────────────────
if (MSVC)
  # Suppress some C warnings from LMDB's C files
  target_compile_options(marketsim PRIVATE /wd4996 /wd4267 /wd4244 /wd4333 /wd4172)
endif()
