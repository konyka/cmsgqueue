cmake_minimum_required(VERSION 3.14)
project(cmsgqueue_tests C)

# Enable CTest integration
include(CTest)

# Discover all test_*.c files in this directory
file(GLOB TEST_SOURCES "test_*.c")

foreach(TEST_SRC ${TEST_SOURCES})
  get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
  add_executable(${TEST_NAME} ${TEST_SRC})
  target_link_libraries(${TEST_NAME} PRIVATE cmsgqueue)
  # Compile as C11, enable warnings as requested
  target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra -Werror -std=c11)
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
  # Expose that we are building a shared component
  target_compile_definitions(${TEST_NAME} PRIVATE CMQ_BUILDING_SHARED _POSIX_C_SOURCE=200809L)
  # Add asCTest entry
  include(CTest)
  if(TARGET ${TEST_NAME})
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
  endif()
endforeach()
