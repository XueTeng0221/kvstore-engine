cmake_minimum_required(VERSION 3.24)

project(kvstore_engine VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)
include(CTest)

option(KVSTORE_BUILD_TESTS "Build tests" ON)
option(KVSTORE_BUILD_BENCHMARKS "Build benchmarks" ON)
option(KVSTORE_BUILD_FUZZERS "Build libFuzzer protocol targets" OFF)
set(KVSTORE_SANITIZERS "" CACHE STRING "Comma-separated sanitizers: address,undefined,thread")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(kvstore_options INTERFACE)
target_compile_options(kvstore_options INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion;-Werror>
)

find_program(KVSTORE_CLANG_FORMAT clang-format)
if(KVSTORE_CLANG_FORMAT)
  file(GLOB_RECURSE KVSTORE_FORMAT_FILES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.cpp
  )
  add_custom_target(format-check
    COMMAND ${KVSTORE_CLANG_FORMAT} --dry-run --Werror ${KVSTORE_FORMAT_FILES}
    VERBATIM
  )
endif()

if(KVSTORE_SANITIZERS)
  string(REPLACE "," ";" _sanitizer_list "${KVSTORE_SANITIZERS}")
  foreach(_sanitizer IN LISTS _sanitizer_list)
    if(NOT _sanitizer MATCHES "^(address|undefined|thread)$")
      message(FATAL_ERROR "Unsupported sanitizer: ${_sanitizer}")
    endif()
  endforeach()
  if("thread" IN_LIST _sanitizer_list AND ("address" IN_LIST _sanitizer_list OR "undefined" IN_LIST _sanitizer_list))
    message(FATAL_ERROR "thread sanitizer must be used in a separate build")
  endif()
  list(JOIN _sanitizer_list "," _sanitizers)
  target_compile_options(kvstore_options INTERFACE -fsanitize=${_sanitizers} -fno-omit-frame-pointer)
  target_link_options(kvstore_options INTERFACE -fsanitize=${_sanitizers} -fno-omit-frame-pointer)
endif()

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
  picosha2
  URL https://github.com/okdshin/PicoSHA2/archive/refs/tags/v1.0.1.tar.gz
  URL_HASH SHA256=9983136544234e573fe07cc1a22fdf978ad7979043e7be2e9082f6d4991ff8a8
)
set(PICOSHA2_TEST OFF CACHE BOOL "" FORCE)
set(PICOSHA2_EXAMPLE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(picosha2)

# Protobuf 3.21.12 (BSD-3-Clause): generated, interoperable sidecar messages.
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(protobuf
  URL https://codeload.github.com/protocolbuffers/protobuf/tar.gz/refs/tags/v21.12
  URL_HASH SHA256=22fdaf641b31655d4b2297f9981fa5203b2866f8332d3c6333f6b0107bb320de
)
FetchContent_MakeAvailable(protobuf)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/kvstore_integration_v1.pb.cc ${CMAKE_CURRENT_BINARY_DIR}/kvstore_integration_v1.pb.h
  COMMAND protobuf::protoc --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
    --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/kvstore_integration_v1.proto
  DEPENDS protobuf::protoc proto/kvstore_integration_v1.proto
  VERBATIM)
add_library(kvstore_integration_proto ${CMAKE_CURRENT_BINARY_DIR}/kvstore_integration_v1.pb.cc)
target_include_directories(kvstore_integration_proto SYSTEM PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(kvstore_integration_proto PUBLIC protobuf::libprotobuf)
if(KVSTORE_SANITIZERS)
  # Instrument generated parsing code without imposing project warnings on it.
  target_compile_options(kvstore_integration_proto PRIVATE -fsanitize=${_sanitizers} -fno-omit-frame-pointer)
endif()

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/kvstore/version.hpp
  @ONLY
)

add_library(kvstore_common
  src/common/crc32.cpp
  src/common/logger.cpp
  src/config/config.cpp
  src/persistence/aof.cpp
  src/persistence/snapshot.cpp
  src/persistence/write_event_queue.cpp
)
target_include_directories(kvstore_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(kvstore_common PUBLIC kvstore_options nlohmann_json::nlohmann_json)

add_library(kvstore_kvcache
  src/kvcache/model.cpp
  src/kvcache/chunk_registry.cpp
  src/kvcache/match_index.cpp
  src/kvcache/tiered_store.cpp
  src/kvcache/policy.cpp
  src/kvcache/request_path.cpp
)
target_include_directories(kvstore_kvcache PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(kvstore_kvcache SYSTEM PRIVATE ${picosha2_SOURCE_DIR})
target_link_libraries(kvstore_kvcache PUBLIC kvstore_common kvstore_options)

add_library(kvstore_integration src/integration/protocol.cpp)
target_include_directories(kvstore_integration PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kvstore_integration PUBLIC kvstore_kvcache kvstore_options kvstore_integration_proto)
add_executable(kvstore_mock_client src/integration/mock_client.cpp)
target_link_libraries(kvstore_mock_client PRIVATE kvstore_integration)

add_library(kvstore_engine_lib
  src/engine/array_engine.cpp
  src/engine/rbtree_engine.cpp
  src/engine/hash_engine.cpp
  src/engine/skiplist_engine.cpp
  src/engine/engine_factory.cpp
  src/engine/engine_util.cpp
  src/command/dispatcher.cpp
  src/protocol/resp.cpp
  src/protocol/native.cpp
  src/protocol/batch.cpp
  src/net/epoll_server.cpp
)
target_include_directories(kvstore_engine_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kvstore_engine_lib PUBLIC kvstore_common kvstore_options)

if(KVSTORE_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "KVSTORE_BUILD_FUZZERS requires Clang")
  endif()
  add_executable(protocol_fuzz tests/fuzz/protocol_fuzz.cpp)
  target_link_libraries(protocol_fuzz PRIVATE kvstore_engine_lib kvstore_options)
  target_compile_options(protocol_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(protocol_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

add_executable(kvstore_server src/server/main.cpp)
target_link_libraries(kvstore_server PRIVATE kvstore_common kvstore_engine_lib kvstore_options)

if(KVSTORE_BUILD_TESTS)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  include(GoogleTest)
  add_test(NAME IntegrationMockClient COMMAND kvstore_mock_client)

  add_executable(kvstore_unit_tests
    tests/unit/common_test.cpp
    tests/unit/config_test.cpp
    tests/unit/engine_contract_test.cpp
    tests/unit/engine_differential_test.cpp
    tests/unit/protocol_test.cpp
    tests/unit/persistence_test.cpp
    tests/unit/kvcache_model_test.cpp
    tests/unit/kvcache_match_test.cpp
    tests/unit/kvcache_tiered_store_test.cpp
    tests/unit/kvcache_policy_test.cpp
    tests/unit/kvcache_request_path_test.cpp
    tests/unit/integration_protocol_test.cpp
  )
  target_link_libraries(kvstore_unit_tests PRIVATE kvstore_common kvstore_engine_lib kvstore_kvcache kvstore_integration GTest::gtest_main kvstore_options)
  target_compile_definitions(kvstore_unit_tests PRIVATE KVSTORE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  gtest_discover_tests(kvstore_unit_tests)

  add_executable(kvstore_integration_tests
    tests/integration/cli_test.cpp
    tests/integration/server_test.cpp
  )
  target_link_libraries(kvstore_integration_tests PRIVATE kvstore_common kvstore_engine_lib GTest::gtest_main kvstore_options)
  target_compile_definitions(kvstore_integration_tests PRIVATE
    KVSTORE_SERVER_PATH="$<TARGET_FILE:kvstore_server>"
    KVSTORE_DEFAULT_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/config/config.json"
  )
  add_dependencies(kvstore_integration_tests kvstore_server)
  gtest_discover_tests(kvstore_integration_tests)
endif()

if(KVSTORE_BUILD_BENCHMARKS)
  add_executable(engine_benchmark benchmarks/engine_benchmark.cpp)
  target_link_libraries(engine_benchmark PRIVATE kvstore_engine_lib kvstore_options)
  add_executable(kvcache_match_benchmark benchmarks/kvcache_match_benchmark.cpp)
  target_link_libraries(kvcache_match_benchmark PRIVATE kvstore_kvcache kvstore_options)
  add_executable(kvcache_policy_benchmark benchmarks/kvcache_policy_benchmark.cpp)
  target_link_libraries(kvcache_policy_benchmark PRIVATE kvstore_kvcache kvstore_options)
endif()
