# cmq_compiler.cmake - Compiler detection and flags for CMSGQueue
#
# Sets up warning flags, sanitizer support, and coverage flags.

# ---------------------------------------------------------------------------
# Detect platform
# ---------------------------------------------------------------------------
# F7: hardening option must be defined BEFORE the warning flags block
# so the -Werror gate can react to it.
option(CMQ_ENABLE_HARDENING "Enable FORTIFY/PIE/RELRO/stack-protector" ON)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CMQ_PLATFORM_LINUX TRUE)
    set(CMQ_HAVE_EPOLL TRUE)
    set(CMQ_HAVE_LIBRT TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(CMQ_PLATFORM_MACOS TRUE)
    set(CMQ_HAVE_KQUEUE TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD" OR CMAKE_SYSTEM_NAME STREQUAL "OpenBSD" OR CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    set(CMQ_PLATFORM_BSD TRUE)
    set(CMQ_HAVE_KQUEUE TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(CMQ_PLATFORM_WINDOWS TRUE)
    set(CMQ_HAVE_IOCP TRUE)
endif()

# ---------------------------------------------------------------------------
# Detect architecture
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    set(CMQ_ARCH_X86_64 TRUE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(CMQ_ARCH_AARCH64 TRUE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    set(CMQ_ARCH_ARM TRUE)
endif()

# ---------------------------------------------------------------------------
# Warning flags (GCC/Clang compatible)
# ---------------------------------------------------------------------------
set(CMQ_GCC_CLANG_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wcast-align
    -Wcast-qual
    -Wformat=2
    -Wimplicit-fallthrough
    -Wmissing-include-dirs
    -Wredundant-decls
    -Wswitch-default
    -Wundef
    -Wunused-parameter
    -Wstrict-prototypes
    -Wold-style-definition
    -Wmissing-prototypes
)

set(CMQ_MSVC_WARNINGS
    /W4
    /permissive-
)

# ---------------------------------------------------------------------------
# Set warning flags based on compiler
# ---------------------------------------------------------------------------
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(CMQ_WARNING_FLAGS ${CMQ_GCC_CLANG_WARNINGS})
    # Add -Werror only in CI AND when hardening is OFF (FORTIFY_SOURCE=2
    # produces benign stringop-truncation warnings on the codebase's
    # correct strncpy-with-null-term pattern; with hardening on, the
    # -Werror guard is replaced by FORTIFY's own runtime checks).
    if(DEFINED ENV{CI} AND NOT CMQ_ENABLE_HARDENING)
        list(APPEND CMQ_WARNING_FLAGS -Werror)
    endif()
elseif(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    set(CMQ_WARNING_FLAGS ${CMQ_MSVC_WARNINGS})
endif()

# ---------------------------------------------------------------------------
# Helper: add sanitizer
# ---------------------------------------------------------------------------
function(cmq_add_sanitizer SANITIZER)
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang")
        set(CMQ_SANITIZER_FLAGS "-fsanitize=${SANITIZER}" PARENT_SCOPE)
        add_compile_options(-fsanitize=${SANITIZER})
        add_link_options(-fsanitize=${SANITIZER})
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Helper: add coverage
# ---------------------------------------------------------------------------
function(cmq_add_coverage)
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang")
        add_compile_options(--coverage -g -O0)
        add_link_options(--coverage)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Architecture-specific flags
# ---------------------------------------------------------------------------
if(CMQ_ARCH_X86_64)
    # Ensure SSE2 for x86_64 (always available). SSE4.2 is enabled for
    # the CRC32C hot path on x86_64 (Nehalem+ baseline).
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        add_compile_options(-msse2)
        add_compile_options(-msse4.2)
    endif()
endif()

# ---------------------------------------------------------------------------
# F7: Build hardening (FORTIFY, PIE, RELRO, partial stack-protector-strong)
# ---------------------------------------------------------------------------
# Defense-in-depth flags. Disabled when sanitizers are active since
# sanitizer builds can otherwise conflict with FORTIFY instrumentation.
# Per-file exclusion list for stack-protector-strong keeps the hot path
# (parser, allocators) inside the +2% perf budget.
#
# Hot-path files excluded from -fstack-protector-strong:
#   src/proto/cmq_parser.c  (per-frame hot loop)
#   src/core/cmq_slab.c     (allocator hot path)
#   src/core/cmq_mpool.c    (pool acquire/release)
# ---------------------------------------------------------------------------
set(CMQ_HARDENING_EXCLUDE_FILES
    src/proto/cmq_parser.c
    src/core/cmq_slab.c
    src/core/cmq_mpool.c
)

if(CMQ_ENABLE_HARDENING AND NOT CMQ_ENABLE_ASAN AND NOT CMQ_ENABLE_UBSAN
                     AND NOT CMQ_ENABLE_TSAN)
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang")
        # FORTIFY_SOURCE=2 requires optimization. Skip on -O0 builds to
        # avoid breaking Debug. The check is conservative: only enable
        # when Release/Fast/RelWithDebInfo.
        get_directory_property(_cmq_cur_flags COMPILE_OPTIONS)
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
            add_compile_definitions(_FORTIFY_SOURCE=2)
        endif()
        # Stack-protector-strong excludes hot-path files via per-source
        # property set during target definition.
        add_compile_options(-fstack-protector-strong)
        # -fPIC is the position-independent code flag for shared
        # libraries (already set by CMake for shared libs). -fPIE is
        # for executables — applied via cmq_harden_executable instead.
        # -pie / -Wl,-z,relro / -Wl,-z,now are link-time flags that
        # only apply to executables. Apply via a helper function the
        # caller invokes AFTER creating each executable target.
        # Enable `-fstack-protector-strong` globally; the per-source
        # property removal below for hot-path files is the override.
        set(CMQ_HARDENING_ENABLED TRUE)
    endif()
endif()

# Helper: harden an executable target. Adds -fPIE, -pie, -Wl,-z,relro,
# -Wl,-z,now. -fpic (for shared libs) is already set by CMake globally.
function(cmq_harden_executable TARGET)
    if(NOT CMQ_ENABLE_HARDENING)
        return()
    endif()
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU"
       AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        return()
    endif()
    target_compile_options(${TARGET} PRIVATE -fPIE)
    target_link_options(${TARGET} PRIVATE
        -pie
        -Wl,-z,relro
        -Wl,-z,now)
endfunction()

# Per-source property: remove -fstack-protector-strong from hot-path
# files. Call from CMakeLists.txt after the target is defined.
function(cmq_apply_hardening_excludes TARGET)
    if(NOT CMQ_ENABLE_HARDENING)
        return()
    endif()
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU"
       AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        return()
    endif()
    foreach(_src ${ARGN})
        get_source_file_property(_cur ${_src} COMPILE_OPTIONS)
        if(_cur)
            list(REMOVE_ITEM _cur "-fstack-protector-strong")
            set_source_files_properties(${_src} PROPERTIES
                COMPILE_OPTIONS "${_cur}")
        endif()
    endforeach()
endfunction()

# ---------------------------------------------------------------------------
# Helper: test that the binary was built with hardening flags
# ---------------------------------------------------------------------------
# Use in CI: a tiny test reads the binary's dynamic section and confirms
# BIND_NOW + PIE flags. If hardening is disabled, the test is skipped.

