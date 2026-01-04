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
    lmdb/mdb.c
    lmdb/midl.c
)

target_include_directories(marketsim
  PUBLIC include
  PUBLIC lmdb
)

# ──────────────────────────────────────────────
# Always build base executable (no gRPC yet)
# ──────────────────────────────────────────────

add_executable(market_sim
    src/main.cpp
)

target_link_libraries(market_sim PRIVATE marketsim)

# ──────────────────────────────────────────────
# Optional gRPC + Protobuf integration
# ──────────────────────────────────────────────

option(MSIM_WITH_GRPC "Enable gRPC integration" OFF)

if(MSIM_WITH_GRPC)
  find_package(Protobuf CONFIG REQUIRED)
  find_package(gRPC CONFIG REQUIRED)

  message(STATUS "Found Protobuf via CONFIG")
  message(STATUS "Found gRPC via CONFIG")

  set(PROTO_FILE "${CMAKE_SOURCE_DIR}/proto/market.proto")
  set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/grpc_gen")
  file(MAKE_DIRECTORY ${GENERATED_DIR})

  # Generate protobuf C++ files
  protobuf_generate_cpp(
      PROTO_SRCS PROTO_HDRS
      ${PROTO_FILE}
      PROTOC_OUT_DIR ${GENERATED_DIR}
  )

  # gRPC plugin (safe for vcpkg / Windows)
  set(GRPC_CPP_PLUGIN $<TARGET_FILE:gRPC::grpc_cpp_plugin>)

  # Generate gRPC client/server bindings
  add_custom_command(
      OUTPUT ${GENERATED_DIR}/market.grpc.pb.cc ${GENERATED_DIR}/market.grpc.pb.h
      COMMAND $<TARGET_FILE:protobuf::protoc>
          --grpc_out=${GENERATED_DIR}
          --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
          --proto_path=${CMAKE_SOURCE_DIR}/proto
          ${PROTO_FILE}
      DEPENDS ${PROTO_FILE}
      COMMENT "Generating gRPC service bindings"
  )

  set(GRPC_SRCS ${GENERATED_DIR}/market.grpc.pb.cc)
  set(GRPC_HDRS ${GENERATED_DIR}/market.grpc.pb.h)

  # Extend marketsim library with protobuf + gRPC
  target_include_directories(marketsim PUBLIC ${GENERATED_DIR})
  target_link_libraries(marketsim PUBLIC
      protobuf::libprotobuf
      gRPC::grpc++
  )

  # Extend the main executable with the generated RPC code
  target_sources(market_sim PRIVATE
      ${PROTO_SRCS} ${PROTO_HDRS}
      ${GRPC_SRCS} ${GRPC_HDRS}
  )

  # Collector server executable
  add_executable(collector_server
      src/collector_server.cpp
      ${PROTO_SRCS} ${PROTO_HDRS}
      ${GRPC_SRCS} ${GRPC_HDRS}
  )

  target_include_directories(collector_server PRIVATE ${GENERATED_DIR})
  target_link_libraries(collector_server PRIVATE
      marketsim
      protobuf::libprotobuf
      gRPC::grpc++
  )
endif()

option(MSIM_BUILD_TESTS "Build unit tests" ON)

if(MSIM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ──────────────────────────────────────────────
# Optional optimization flags
# ──────────────────────────────────────────────

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported)
  set_property(TARGET marketsim PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  set_property(TARGET market_sim PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# ──────────────────────────────────────────────
# Optional extra logging
# ──────────────────────────────────────────────

option(ENABLE_STATS "Enable extra stats logging" ON)
if(ENABLE_STATS)
  target_compile_definitions(marketsim PUBLIC ENABLE_STATS=1)
  target_compile_definitions(market_sim PUBLIC ENABLE_STATS=1)
endif()

# ──────────────────────────────────────────────
# Windows-specific LMDB warnings
# ──────────────────────────────────────────────
if(MSVC)
  target_compile_options(marketsim PRIVATE
      /wd4996 /wd4267 /wd4244 /wd4333 /wd4172
  )
endif()
