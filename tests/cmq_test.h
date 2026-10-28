// Minimal single-header C test framework for C11 (no dynamic allocation)
// - Provides TEST(suite, name) macro and a per-translation-unit test registry
// - Provides ASSERT macros and TEST_MAIN() to run tests in this TU
// - Colored output when stdout is a tty
// - Reports: test name, pass/fail, file:line on failure
// - Returns 0 on success, 1 on any failure
// - Uses __attribute__((constructor)) for auto-registration
//
// This header is self-contained and does not rely on any external framework.

#ifndef CMQ_TEST_H
#define CMQ_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <setjmp.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>

// Test descriptor (one per test body)
typedef struct cmq_test cmq_test_t;
struct cmq_test {
    const char *suite;
    const char *name;
    void (*fn)(void);
    cmq_test_t *next;
};

// Per-translation-unit head for tests in this TU
static cmq_test_t *cmq_tests_head = NULL;

// Register a test into the current TU's list (called from constructor in TEST macro)
static inline void cmq_test_register_head(cmq_test_t *t) {
    t->next = cmq_tests_head;
    cmq_tests_head = t;
}

// Internal: current test context (for failure messages and timing)
static const char *cmq_current_suite = NULL;
static const char *cmq_current_name  = NULL;
static jmp_buf cmq_jmp_buf;
static int cmq_color_enabled __attribute__((unused)) = 0;

// Run a shell command for test setup/cleanup (mkdir, rm -rf, openssl req ...).
// The exit status is intentionally ignored: callers that depend on the result
// check the produced files instead, and this keeps -Wunused-result quiet.
static inline void cmq_test_sh(const char *cmd) {
    int rc = system(cmd);
    (void)rc;
}

// socket(2) with O_NONBLOCK set before the caller connects. SOCK_NONBLOCK is a
// Linux extension (macOS has no such flag), and fcntl before connect() gives
// the same behaviour: connect returns EINPROGRESS instead of blocking.
#include <fcntl.h>
#include <sys/socket.h>
static inline int cmq_test_nonblock_socket(int domain, int type) {
    int fd = socket(domain, type, 0);
    if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Wait until a non-blocking connect() has completed. On Linux loopback it is
// usually done at once, but on macOS the socket is still unconnected right
// after EINPROGRESS and an immediate write fails with ENOTCONN.
#include <poll.h>
static inline int cmq_test_wait_connected(int fd, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    int rc;
    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);
    if (rc <= 0) return -1;
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0) return -1;
    return so_error == 0 ? 0 : -1;
}

// 1 if the host can bind the given loopback address. Linux routes all of
// 127.0.0.0/8 to lo; macOS only configures 127.0.0.1, so tests that need a
// second source address must skip there instead of failing.
#include <netinet/in.h>
#include <arpa/inet.h>
static inline int cmq_test_can_bind_loopback(const char *ip) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { close(fd); return 0; }
    int ok = bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0;
    close(fd);
    return ok;
}

// Failure reporter: reports file/line and expression, then long-jumps back to test harness
static inline void cmq_test_fail(const char *file, int line, const char *expr) {
    if (cmq_current_suite && cmq_current_name) {
        fprintf(stderr, "FAILED: %s.%s %s:%d: %s\n",
                cmq_current_suite, cmq_current_name, file, line, expr);
    } else {
        fprintf(stderr, "FAILED: %s:%d: %s\n", file, line, expr);
    }
    longjmp(cmq_jmp_buf, 1);
}

// ASSERT helpers (no allocations, type-generic via simple casts)
#define ASSERT(cond) do { if(!(cond)) cmq_test_fail(__FILE__, __LINE__, #cond); } while(0)
#define ASSERT_NULL(p) do { if((p) != NULL) cmq_test_fail(__FILE__, __LINE__, #p" is not NULL"); } while(0)
#define ASSERT_NOT_NULL(p) do { if((p) != NULL) ; else cmq_test_fail(__FILE__, __LINE__, #p" is NULL"); } while(0)
// Equality assertions (use 64-bit cast to cover common integer types)
#define ASSERT_EQ(a, b) do { if(((long long)(a)) != ((long long)(b))) cmq_test_fail(__FILE__, __LINE__, #a" != "#b); } while(0)
#define ASSERT_STR_EQ(a, b) do { if(strcmp((const char*)(a), (const char*)(b)) != 0) cmq_test_fail(__FILE__, __LINE__, #a" != "#b); } while(0)
#define ASSERT_MEM_EQ(a, b, len) do { if(memcmp((const void*)(a), (const void*)(b), (size_t)(len)) != 0) cmq_test_fail(__FILE__, __LINE__, #a" mem != " #b); } while(0)
#define ASSERT_TRUE(cond) ASSERT(cond)

// TEST macro: declare body, register static descriptor, register at ctor, and allow body afterwards
#define TEST(suite, name) \
    static void test_##suite##_##name(void); \
    static cmq_test_t cmq_test_entry_##suite##_##name = { #suite, #name, test_##suite##_##name, NULL }; \
    static void __attribute__((constructor)) cmq_register_##suite##_##name(void) { cmq_test_register_head(&cmq_test_entry_##suite##_##name); } \
    static void test_##suite##_##name(void)

// Run all tests defined in this TU. Returns 0 on success, 1 if any test failed.
#define TEST_MAIN() \
int main(void) { \
    cmq_color_enabled = isatty((int)STDOUT_FILENO) ? 1 : 0; \
    volatile int tests_run = 0; \
    volatile int tests_failed = 0; \
    clock_t wall_start = clock(); \
    cmq_current_suite = NULL; cmq_current_name = NULL; \
    cmq_test_t *cmq_iter = cmq_tests_head; \
    while (cmq_iter != NULL) { \
        cmq_test_t *t = cmq_iter; \
        cmq_iter = cmq_iter->next; \
        cmq_current_suite = t->suite; \
        cmq_current_name  = t->name; \
        clock_t tstart = clock(); \
        int failed = 0; \
        if (setjmp(cmq_jmp_buf) == 0) { \
            t->fn(); \
        } else { \
            failed = 1; \
        } \
        clock_t tend = clock(); \
        double dt = (double)(tend - tstart) / CLOCKS_PER_SEC; \
        tests_run++; \
        if (!failed) { \
            if (cmq_color_enabled) printf("\033[32mPASS\033[0m"); else printf("PASS"); \
            printf("  %s.%s (%gs)\n", t->suite, t->name, dt); \
        } else { \
            tests_failed++; \
            if (cmq_color_enabled) printf("\033[31mFAIL\033[0m"); else printf("FAIL"); \
            printf("  %s.%s (%gs)\n", t->suite, t->name, dt); \
        } \
    } \
    double total = (double)(clock() - wall_start) / CLOCKS_PER_SEC; \
    printf("Total: %d tests, %d failures in %.6fs\n", (int)tests_run, (int)tests_failed, total); \
    return ((int)tests_failed ? 1 : 0); \
}

#endif // CMQ_TEST_H
