#define _POSIX_C_SOURCE 200809L
#include "cmq_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

#define FLOOR_MSG_PER_SEC    25000
#define FLOOR_P99_LATENCY_US 200
#define HARD_DROPPED_LIMIT   5000

static int run_benchmark_and_capture(char *out, size_t out_len) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execlp("./examples/benchmark", "./examples/benchmark",
               "-c", "10", "-n", "10000", "-t", "1", "-j",
               (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    size_t n = 0;
    ssize_t r;
    while (n + 1 < out_len && (r = read(pipefd[0], out + n, out_len - n - 1)) > 0) {
        n += (size_t)r;
    }
    out[n] = '\0';
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

static int extract_double(const char *json, const char *key, double *out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ') p++;
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return -1;
    *out = v;
    return 0;
}

TEST(bench, publish_path_floor) {
    char buf[16384];
    int rc = run_benchmark_and_capture(buf, sizeof(buf));
    if (rc != 0) {
        fprintf(stderr, "  benchmark exited non-zero (%d); output:\n%s\n", rc, buf);
        ASSERT(0);
    }

    char *json_start = strrchr(buf, '{');
    if (!json_start) {
        fprintf(stderr, "  no JSON line found in output:\n%s\n", buf);
        ASSERT(0);
    }

    double msg_per_sec = 0.0;
    double p99_us = 0.0;
    double dropped = 0.0;

    ASSERT_EQ(extract_double(json_start, "msg_per_sec", &msg_per_sec), 0);
    ASSERT_EQ(extract_double(json_start, "p99_us", &p99_us), 0);
    ASSERT_EQ(extract_double(json_start, "dropped", &dropped), 0);

    printf("  baseline: msg_per_sec=%.0f p99=%.1f us dropped=%.0f\n",
           msg_per_sec, p99_us, dropped);

    ASSERT(msg_per_sec >= (double)FLOOR_MSG_PER_SEC);
    ASSERT(p99_us <= (double)FLOOR_P99_LATENCY_US);
    ASSERT(dropped <= (double)HARD_DROPPED_LIMIT);
}

TEST_MAIN()