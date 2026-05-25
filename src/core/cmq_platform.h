#ifndef CMQ_PLATFORM_H
#define CMQ_PLATFORM_H

/* ---- Operating System Detection ---- */
#if defined(_WIN32) || defined(_WIN64)
    #define CMQ_OS_WINDOWS  1
    #define CMQ_OS_UNIX     0
#elif defined(__linux__)
    #define CMQ_OS_LINUX    1
    #define CMQ_OS_UNIX     1
    #define CMQ_OS_MACOS    0
#elif defined(__APPLE__) && defined(__MACH__)
    #define CMQ_OS_MACOS    1
    #define CMQ_OS_UNIX     1
    #define CMQ_OS_LINUX    0
#elif defined(__FreeBSD__)
    #define CMQ_OS_FREEBSD  1
    #define CMQ_OS_UNIX     1
#elif defined(__OpenBSD__)
    #define CMQ_OS_OPENBSD  1
    #define CMQ_OS_UNIX     1
#elif defined(__NetBSD__)
    #define CMQ_OS_NETBSD   1
    #define CMQ_OS_UNIX     1
#else
    #error "Unsupported operating system"
#endif

/* ---- Architecture Detection ---- */
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    #define CMQ_ARCH_X86_64  1
    #define CMQ_ARCH_64BIT   1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define CMQ_ARCH_AARCH64 1
    #define CMQ_ARCH_64BIT   1
#elif defined(__arm__) || defined(_M_ARM)
    #define CMQ_ARCH_ARM     1
    #define CMQ_ARCH_32BIT   1
#elif defined(__i386__) || defined(_M_IX86)
    #define CMQ_ARCH_X86     1
    #define CMQ_ARCH_32BIT   1
#else
    #error "Unsupported architecture"
#endif

/* ---- Compiler Detection ---- */
#if defined(__clang__)
    #define CMQ_COMPILER_CLANG 1
    #define CMQ_COMPILER_GCC   0
    #define CMQ_COMPILER_MSVC  0
#elif defined(__GNUC__)
    #define CMQ_COMPILER_GCC   1
    #define CMQ_COMPILER_CLANG 0
    #define CMQ_COMPILER_MSVC  0
#elif defined(_MSC_VER)
    #define CMQ_COMPILER_MSVC  1
    #define CMQ_COMPILER_GCC   0
    #define CMQ_COMPILER_CLANG 0
#else
    #define CMQ_COMPILER_UNKNOWN 1
    #define CMQ_COMPILER_GCC   0
    #define CMQ_COMPILER_MSVC  0
    #define CMQ_COMPILER_CLANG 0
#endif

/* ---- Compiler Feature Detection ---- */
#if defined(__has_builtin)
    #define CMQ_HAS_BUILTIN(x) __has_builtin(x)
#else
    #define CMQ_HAS_BUILTIN(x) 0
#endif

/* ---- Export/Import for shared library ---- */
#if defined(CMQ_BUILDING_SHARED)
    #if CMQ_OS_WINDOWS
        #define CMQ_API __declspec(dllexport)
    #else
        #define CMQ_API __attribute__((visibility("default")))
    #endif
#elif defined(CMQ_SHARED)
    #if CMQ_OS_WINDOWS
        #define CMQ_API __declspec(dllimport)
    #else
        #define CMQ_API
    #endif
#else
    #define CMQ_API
#endif

/* ---- Inline keyword ---- */
#if CMQ_COMPILER_MSVC
    #define cmq_inline __inline
#else
    #define cmq_inline inline __attribute__((always_inline))
#endif

/* ---- Endianness ---- */
#if defined(__BYTE_ORDER__)
    #if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        #define CMQ_BIG_ENDIAN    1
        #define CMQ_LITTLE_ENDIAN 0
    #else
        #define CMQ_BIG_ENDIAN    0
        #define CMQ_LITTLE_ENDIAN 1
    #endif
#elif CMQ_OS_WINDOWS
    #define CMQ_BIG_ENDIAN    0
    #define CMQ_LITTLE_ENDIAN 1
#else
    #define CMQ_BIG_ENDIAN    0
    #define CMQ_LITTLE_ENDIAN 1
#endif

/* ---- Branch prediction hints ---- */
#if CMQ_HAS_BUILTIN(__builtin_expect)
    #define CMQ_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define CMQ_UNLIKELY(x) __builtin_expect(!!(x), 0)
#elif CMQ_COMPILER_MSVC
    #define CMQ_LIKELY(x)   (x)
    #define CMQ_UNLIKELY(x) (x)
#endif

/* ---- Container of macro ---- */
#define cmq_container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#endif
