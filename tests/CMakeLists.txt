cmake_minimum_required(VERSION 3.14)
project(cmsgqueue_tests C)

# Enable CTest integration
include(CTest)

# Discover all test_*.c files in this directory
file(GLOB TEST_SOURCES "test_*.c")

set(CMQ_TCP_TESTS
  test_coro_integration
  test_integration
  test_phase2
  test_request_reply
  test_server
  test_server_ops
  test_stress
  test_worker
)

foreach(TEST_SRC ${TEST_SOURCES})
  get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
  add_executable(${TEST_NAME} ${TEST_SRC})
  target_link_libraries(${TEST_NAME} PRIVATE cmsgqueue)
  # Compile as C11, enable warnings as requested
  # Note: -Werror is removed when F7 hardening is on (FORTIFY_SOURCE=2
  # produces benign stringop-truncation warnings on correct patterns).
  if(CMQ_ENABLE_HARDENING)
    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra -std=c11)
  else()
    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra -Werror -std=c11)
  endif()
  # Optional sanitizers
  if(DEFINED CMQ_ENABLE_ASAN AND CMQ_ENABLE_ASAN)
    target_compile_options(${TEST_NAME} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_link_options(${TEST_NAME} PRIVATE -fsanitize=address)
    endif()
  endif()
  if(DEFINED CMQ_ENABLE_UBSAN AND CMQ_ENABLE_UBSAN)
    target_compile_options(${TEST_NAME} PRIVATE -fsanitize=undefined)
    if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_link_options(${TEST_NAME} PRIVATE -fsanitize=undefined)
    endif()
  endif()
  cmq_harden_executable(${TEST_NAME})
  # Expose that we are building a shared component
  target_compile_definitions(${TEST_NAME} PRIVATE CMQ_BUILDING_SHARED _POSIX_C_SOURCE=200809L)
  # Add asCTest entry
  include(CTest)
  if(TARGET ${TEST_NAME})
    if("${TEST_NAME}" STREQUAL "test_bench_regression")
      option(CMQ_ENABLE_BENCH "Enable perf regression gate (opt-in)" OFF)
      if(CMQ_ENABLE_BENCH)
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
        set_tests_properties(${TEST_NAME} PROPERTIES LABELS "BENCH")
      endif()
    else()
      add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endif()
    list(FIND CMQ_TCP_TESTS ${TEST_NAME} CMQ_TCP_TEST_INDEX)
    if(NOT CMQ_TCP_TEST_INDEX EQUAL -1)
      set_tests_properties(${TEST_NAME} PROPERTIES RESOURCE_LOCK cmq_tcp_server)
    endif()
  endif()
endforeach()
