cmake_minimum_required(VERSION 3.24)

project(kvstore_engine VERSION 0.1.0 LANGUAGES C CXX)

include(FetchContent)
include(CTest)

set(KVSTORE_FETCHCONTENT_MIRROR "" CACHE STRING
    "Optional GitHub mirror base URL, used after the canonical URL fails")

function(kvstore_github_urls output original)
  set(urls "${original}")
  if(KVSTORE_FETCHCONTENT_MIRROR)
    string(REGEX REPLACE "/$" "" mirror "${KVSTORE_FETCHCONTENT_MIRROR}")
    string(REPLACE "https://github.com" "${mirror}" fallback "${original}")
    string(REPLACE "https://codeload.github.com" "${mirror}" fallback "${fallback}")
    list(APPEND urls "${fallback}")
  endif()
  set(${output} "${urls}" PARENT_SCOPE)
endfunction()

option(KVSTORE_BUILD_TESTS "Build tests" ON)
option(KVSTORE_BUILD_BENCHMARKS "Build benchmarks" ON)
option(KVSTORE_BUILD_FUZZERS "Build libFuzzer protocol targets" OFF)
option(KVSTORE_BUILD_GPU_TESTS "Register framework GPU runtime tests" OFF)
option(KVSTORE_ENABLE_NTYCO "Build the pinned third-party NtyCo runtime" ON)
option(KVSTORE_ACCEPT_NTYCO_RESTRICTED_LICENSE
       "Acknowledge NtyCo upstream has no permissive license" OFF)
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

kvstore_github_urls(JSON_URLS "https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz")
FetchContent_Declare(nlohmann_json URL ${JSON_URLS}
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
FetchContent_MakeAvailable(nlohmann_json)

kvstore_github_urls(PICOSHA2_URLS "https://github.com/okdshin/PicoSHA2/archive/refs/tags/v1.0.1.tar.gz")
FetchContent_Declare(picosha2 URL ${PICOSHA2_URLS}
  URL_HASH SHA256=9983136544234e573fe07cc1a22fdf978ad7979043e7be2e9082f6d4991ff8a8)
set(PICOSHA2_TEST OFF CACHE BOOL "" FORCE)
set(PICOSHA2_EXAMPLE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(picosha2)

if(KVSTORE_ENABLE_NTYCO AND UNIX)
  if(NOT KVSTORE_ACCEPT_NTYCO_RESTRICTED_LICENSE)
    message(FATAL_ERROR
      "NtyCo has no upstream license file; set KVSTORE_ACCEPT_NTYCO_RESTRICTED_LICENSE=ON only for local evaluation")
  endif()
  FetchContent_Declare(ntyco
    GIT_REPOSITORY https://github.com/wangbojing/NtyCo.git
    GIT_TAG 72ab5fd04f0c228f464f160aaa521bb791b34aa
    GIT_SHALLOW FALSE)
  FetchContent_MakeAvailable(ntyco)
  set(NTYCO_WAKEUP_MARKER "${ntyco_BINARY_DIR}/.kvstore_scheduler_cancel_v2_patched")
  if(EXISTS "${NTYCO_WAKEUP_MARKER}")
    file(READ "${ntyco_SOURCE_DIR}/core/nty_schedule.c" ntyco_scheduler_source)
    string(FIND "${ntyco_scheduler_source}" "SYS_read" ntyco_wakeup_source_offset)
    string(FIND "${ntyco_scheduler_source}" "sched->eventfd = -1" ntyco_eventfd_close_offset)
    string(FIND "${ntyco_scheduler_source}" "eventlist[idx].data.fd == sched->eventfd" ntyco_eventfd_filter_offset)
    string(FIND "${ntyco_scheduler_source}" "stop_requested" ntyco_cancel_source_offset)
    string(FIND "${ntyco_scheduler_source}" "nty_schedule_cancel_all" ntyco_cancel_all_offset)
    string(FIND "${ntyco_scheduler_source}" "EPOLL_CTL_DEL" ntyco_cancel_epoll_offset)
    if(ntyco_wakeup_source_offset EQUAL -1 OR ntyco_eventfd_close_offset EQUAL -1 OR
       ntyco_eventfd_filter_offset EQUAL -1 OR ntyco_cancel_source_offset EQUAL -1 OR
       ntyco_cancel_all_offset EQUAL -1 OR ntyco_cancel_epoll_offset EQUAL -1)
      file(REMOVE "${NTYCO_WAKEUP_MARKER}")
    endif()
  endif()
  if(NOT EXISTS "${NTYCO_WAKEUP_MARKER}")
    file(READ "${ntyco_SOURCE_DIR}/core/nty_schedule.c" ntyco_scheduler_source)
    string(REPLACE "\n\tnty_schedule_free(sched);\n\t\n\treturn ;" "\n\t\n\treturn ;" ntyco_scheduler_source "${ntyco_scheduler_source}")
    file(WRITE "${ntyco_SOURCE_DIR}/core/nty_schedule.c" "${ntyco_scheduler_source}")
    string(FIND "${ntyco_scheduler_source}" "nty_schedule_cancel_all" ntyco_already_patched)
    string(FIND "${ntyco_scheduler_source}" "\n\tnty_schedule_free(sched);\n\t\n\treturn ;" ntyco_scheduler_owns_free)
    if(ntyco_already_patched GREATER -1 AND ntyco_scheduler_owns_free EQUAL -1)
      file(WRITE "${NTYCO_WAKEUP_MARKER}" "kvstore scheduler wakeup patch applied\n")
    else()
      execute_process(
        COMMAND patch -N -p1 -i "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ntyco_scheduler_cancel.patch"
        WORKING_DIRECTORY "${ntyco_SOURCE_DIR}"
        RESULT_VARIABLE ntyco_patch_result
        OUTPUT_VARIABLE ntyco_patch_output
        ERROR_VARIABLE ntyco_patch_error)
      if(NOT ntyco_patch_result EQUAL 0)
        message(FATAL_ERROR "failed to patch pinned NtyCo scheduler: ${ntyco_patch_output}${ntyco_patch_error}")
      endif()
      file(WRITE "${NTYCO_WAKEUP_MARKER}" "kvstore scheduler wakeup patch applied\n")
    endif()
  endif()
  add_library(ntyco_runtime STATIC
    ${ntyco_SOURCE_DIR}/core/nty_coroutine.c
    ${ntyco_SOURCE_DIR}/core/nty_epoll.c
    ${ntyco_SOURCE_DIR}/core/nty_schedule.c
    ${ntyco_SOURCE_DIR}/core/nty_socket.c)
  target_include_directories(ntyco_runtime PUBLIC ${ntyco_SOURCE_DIR}/core)
  target_compile_options(ntyco_runtime PRIVATE -w)
  target_link_libraries(ntyco_runtime PUBLIC pthread dl)
endif()

# Protobuf 3.21.12 (BSD-3-Clause): generated, interoperable sidecar messages.
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
kvstore_github_urls(PROTOBUF_URLS "https://codeload.github.com/protocolbuffers/protobuf/tar.gz/refs/tags/v21.12")
FetchContent_Declare(protobuf
  URL ${PROTOBUF_URLS}
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
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/python/kvstore_integration_v1_pb2.py
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/python
  COMMAND protobuf::protoc --python_out=${CMAKE_CURRENT_BINARY_DIR}/python
    --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/kvstore_integration_v1.proto
  DEPENDS protobuf::protoc proto/kvstore_integration_v1.proto
  VERBATIM)
add_custom_target(kvstore_python_proto DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/python/kvstore_integration_v1_pb2.py)
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

add_library(kvstore_integration src/integration/protocol.cpp src/integration/uds_bridge.cpp src/integration/sglang/hicache_storage.cpp)
target_include_directories(kvstore_integration PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kvstore_integration PUBLIC kvstore_kvcache kvstore_options kvstore_integration_proto)
add_executable(kvstore_mock_client src/integration/mock_client.cpp)
target_link_libraries(kvstore_mock_client PRIVATE kvstore_integration)
add_subdirectory(src/integration/vllm)

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
  src/net/io_uring_server.cpp
  src/net/ntyco_server.cpp
  src/replication/handshake.cpp
  src/replication/backlog.cpp
  src/replication/sync.cpp
  src/replication/frame.cpp
  src/replication/live_sync.cpp
  src/replication/executor.cpp
  src/replication/io_uring_executor.cpp
)
target_include_directories(kvstore_engine_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kvstore_engine_lib PUBLIC kvstore_common kvstore_options)
if(TARGET ntyco_runtime)
  target_sources(kvstore_engine_lib PRIVATE src/replication/ntyco_bridge.c)
  target_include_directories(kvstore_engine_lib SYSTEM PRIVATE ${ntyco_SOURCE_DIR}/core)
  target_compile_definitions(kvstore_engine_lib PRIVATE KVSTORE_HAVE_NTYCO=1)
  target_link_libraries(kvstore_engine_lib PUBLIC ntyco_runtime)
endif()

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
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  kvstore_github_urls(GTEST_URLS "https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz")
  FetchContent_Declare(googletest URL ${GTEST_URLS}
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  include(GoogleTest)
  add_subdirectory(tests/integration/vllm)
  add_executable(kvstore_uds_bridge_fixture tests/integration/uds_bridge_fixture.cpp)
  target_link_libraries(kvstore_uds_bridge_fixture PRIVATE kvstore_integration kvstore_options)
  add_dependencies(kvstore_uds_bridge_fixture kvstore_python_proto)
  add_test(NAME UdsBridgeLive COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_CURRENT_SOURCE_DIR}/tests/python -p test_uds_bridge_live.py -v)
  set_tests_properties(UdsBridgeLive PROPERTIES TIMEOUT 45 ENVIRONMENT
    "KVSTORE_UDS_FIXTURE=$<TARGET_FILE:kvstore_uds_bridge_fixture>;KVSTORE_PYTHON_PROTO_DIR=${CMAKE_CURRENT_BINARY_DIR}/python")
  add_test(NAME UdsTransport COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_CURRENT_SOURCE_DIR}/tests/python -p test_uds_transport.py -v)
  set_tests_properties(UdsTransport PROPERTIES TIMEOUT 20)
  add_test(NAME IntegrationMockClient COMMAND kvstore_mock_client)
  if(KVSTORE_BUILD_GPU_TESTS)
    add_test(NAME VllmGpuRuntime COMMAND ${Python3_EXECUTABLE} -m unittest discover
      -s ${CMAKE_CURRENT_SOURCE_DIR}/tests/python -p test_vllm_gpu_runtime.py -v)
    set_tests_properties(VllmGpuRuntime PROPERTIES TIMEOUT 120)
  endif()

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
    tests/unit/replication_test.cpp
    tests/unit/sglang_hicache_storage_test.cpp
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
  add_executable(replication_executor_benchmark benchmarks/replication_executor_benchmark.cpp)
  target_link_libraries(replication_executor_benchmark PRIVATE kvstore_engine_lib kvstore_options)
endif()
