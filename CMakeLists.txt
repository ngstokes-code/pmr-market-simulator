
cmake_minimum_required(VERSION 3.15)
project(pmr_market_simulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_library(marketsim
    src/order_book.cpp
    src/simulator.cpp
    src/storage.cpp
    src/pmr_utils.cpp
)

target_include_directories(marketsim PUBLIC include)

add_executable(market_sim src/main.cpp)
target_link_libraries(market_sim PRIVATE marketsim)

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported)
  set_property(TARGET market_sim PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  set_property(TARGET marketsim PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

option(ENABLE_STATS "Enable extra stats logging" ON)
if(ENABLE_STATS)
  target_compile_definitions(marketsim PUBLIC ENABLE_STATS=1)
  target_compile_definitions(market_sim PUBLIC ENABLE_STATS=1)
endif()
