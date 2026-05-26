# CMSGQueue - High-Performance Message Queue in Pure C
# Copyright (c) 2025 CMSGQueue Contributors
# Licensed under the Apache License, Version 2.0

cmake_minimum_required(VERSION 3.14)

project(cmsgqueue
    VERSION 0.1.0
    DESCRIPTION "High-performance message queue in pure C"
    LANGUAGES C ASM
)

# ---------------------------------------------------------------------------
# C standard - target C11 for atomics, compatible with C99-C23 compilers
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Build type defaults to Debug
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
include(cmake/cmq_compiler.cmake)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(CMQ_BUILD_TESTS "Build unit tests" ON)
option(CMQ_BUILD_EXAMPLES "Build example programs" ON)
option(CMQ_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(CMQ_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(CMQ_ENABLE_COVERAGE "Enable code coverage" OFF)
option(CMQ_STATIC "Build static library instead of shared" OFF)

# ---------------------------------------------------------------------------
# Sanitizers
# ---------------------------------------------------------------------------
if(CMQ_ENABLE_ASAN)
    cmq_add_sanitizer(address)
endif()
if(CMQ_ENABLE_UBSAN)
    cmq_add_sanitizer(undefined)
endif()
if(CMQ_ENABLE_COVERAGE)
    cmq_add_coverage()
endif()

# ---------------------------------------------------------------------------
# Library sources
# ---------------------------------------------------------------------------
set(CMQ_CORE_SOURCES
    src/core/cmq_mpool.c
    src/core/cmq_slab.c
    src/core/cmq_log.c
    src/core/cmq_thread.c
    src/core/cmq_queue.c
)

set(CMQ_CORO_SOURCES
    src/coro/cmq_coro.c
)

if(CMQ_ARCH_X86_64)
    list(APPEND CMQ_CORO_SOURCES src/coro/arch/cmq_coro_ctx_x86_64.S)
elseif(CMQ_ARCH_AARCH64)
    list(APPEND CMQ_CORO_SOURCES src/coro/arch/cmq_coro_ctx_aarch64.S)
endif()

set(CMQ_NET_SOURCES
    src/net/cmq_ev.c
)

set(CMQ_PROTO_SOURCES
    src/proto/cmq_parser.c
)

set(CMQ_SERVER_SOURCES
    src/server/cmq_server.c
    src/server/cmq_client.c
    src/server/cmq_sublist.c
    src/server/cmq_config.c
)

set(CMQ_STORE_SOURCES
    src/store/cmq_store.c
    src/store/cmq_stream.c
    src/store/cmq_filestore.c
)

set(CMQ_CLUSTER_SOURCES
    src/cluster/cmq_cluster.c
    src/cluster/cmq_route.c
    src/cluster/cmq_gateway.c
    src/cluster/cmq_leaf.c
)

# Collect all sources
set(CMQ_ALL_SOURCES
    ${CMQ_CORE_SOURCES}
    ${CMQ_CORO_SOURCES}
    ${CMQ_NET_SOURCES}
    ${CMQ_PROTO_SOURCES}
    ${CMQ_SERVER_SOURCES}
    ${CMQ_STORE_SOURCES}
    ${CMQ_CLUSTER_SOURCES}
)

# ---------------------------------------------------------------------------
# Create library target
# ---------------------------------------------------------------------------
if(CMQ_STATIC)
    add_library(cmsgqueue STATIC ${CMQ_ALL_SOURCES})
else()
    add_library(cmsgqueue SHARED ${CMQ_ALL_SOURCES})
endif()

target_include_directories(cmsgqueue
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/coro>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/net>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/proto>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/server>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/store>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/cluster>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(cmsgqueue PUBLIC c_std_11)
target_compile_options(cmsgqueue PRIVATE ${CMQ_WARNING_FLAGS})

# Platform-specific link libraries
if(UNIX)
    target_link_libraries(cmsgqueue PRIVATE pthread)
endif()
if(CMQ_HAVE_LIBRT)
    target_link_libraries(cmsgqueue PRIVATE rt)
endif()

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
if(CMQ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CMQ_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
