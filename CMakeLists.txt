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
option(CMQ_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
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
if(CMQ_ENABLE_TSAN)
    cmq_add_sanitizer(thread)
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
    src/core/cmq_queue.c
    src/core/cmq_crc32c.c
    src/server/cmq_password.c
    src/server/cmq_trace.c
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
    src/proto/cmq_compress.c
)

set(CMQ_SERVER_SOURCES
    src/server/cmq_server.c
    src/server/cmq_config.c
    src/server/cmq_sublist.c
    src/server/cmq_sublist_persist.c
    src/server/cmq_subject_rl.c
)

set(CMQ_STORE_SOURCES
    src/store/cmq_store.c
    src/store/cmq_stream.c
    src/store/cmq_filestore.c
)

set(CMQ_CLUSTER_SOURCES
    src/cluster/cmq_cluster.c
    src/cluster/cmq_route.c
    src/cluster/cmq_route_tls.c
    src/cluster/cmq_route_tls_sess.c
    src/cluster/cmq_leaf.c
    src/cluster/cmq_gateway.c
    src/cluster/cmq_blocklist.c
)

set(CMQ_ENTERPRISE_SOURCES
    src/enterprise/cmq_account.c
    src/enterprise/cmq_tls.c
    src/enterprise/cmq_mqtt.c
    src/enterprise/cmq_mqtt_server.c
    src/enterprise/cmq_audit.c
    src/enterprise/cmq_quota.c
    src/enterprise/cmq_acl.c
    src/enterprise/cmq_ws.c
    src/enterprise/cmq_tls_session_cache.c
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
    ${CMQ_ENTERPRISE_SOURCES}
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
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/enterprise>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/util>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(cmsgqueue PUBLIC c_std_11)
target_compile_options(cmsgqueue PRIVATE ${CMQ_WARNING_FLAGS})

# F7: Exclude hot-path TUs from -fstack-protector-strong to keep the
# per-frame parser and per-allocation allocators inside the +2% perf
# budget. The compiled-in canary would otherwise add a measurable
# tax on these functions.
cmq_apply_hardening_excludes(${CMQ_CORE_SOURCES} ${CMQ_PROTO_SOURCES})

# Platform-specific link libraries
if(UNIX)
    target_link_libraries(cmsgqueue PRIVATE pthread)
endif()
if(CMQ_HAVE_LIBRT)
    target_link_libraries(cmsgqueue PRIVATE rt)
endif()
find_package(OpenSSL REQUIRED)
target_link_libraries(cmsgqueue PUBLIC OpenSSL::Crypto OpenSSL::SSL)
target_compile_definitions(cmsgqueue PUBLIC CMQ_TLS_OPENSSL=1)

# F2: zstd compression (system libzstd 1.5+).
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED libzstd)
target_include_directories(cmsgqueue PRIVATE ${ZSTD_INCLUDE_DIRS})
target_link_libraries(cmsgqueue PRIVATE ${ZSTD_LIBRARIES})

# v0.5.22: WS permessage-deflate (RFC 7692) needs zlib (system libz).
find_package(ZLIB)
if(ZLIB_FOUND)
    target_link_libraries(cmsgqueue PRIVATE ZLIB::ZLIB)
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
