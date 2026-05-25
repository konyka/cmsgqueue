# cmq_compiler.cmake - Compiler detection and flags for CMSGQueue
#
# Sets up warning flags, sanitizer support, and coverage flags.

# ---------------------------------------------------------------------------
# Detect platform
# ---------------------------------------------------------------------------
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
    # Add -Werror only in CI
    if(DEFINED ENV{CI})
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
    # Ensure SSE2 for x86_64 (always available)
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        add_compile_options(-msse2)
    endif()
endif()
