/* P2: WAL append throughput benchmark. Counts appends per second. */

#define _POSIX_C_SOURCE 200809L
#include "cmq_test.h"
#include "cmq_filestore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WAL_BENCH_DIR "/tmp/cmq-test-wal-bench"
#define WAL_BENCH_N   10000

TEST(persist_unit, wal_append_throughput) {
    int rc __attribute__((unused)) = system("rm -rf " WAL_BENCH_DIR " && mkdir -p " WAL_BENCH_DIR);
    (void)rc;

    cmq_filestore_t *fs = cmq_filestore_create(WAL_BENCH_DIR, "bench");
    ASSERT_NOT_NULL(fs);

    uint8_t payload[128];
    memset(payload, 0xAB, sizeof(payload));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t last_seq = 0;
    for (int i = 0; i < WAL_BENCH_N; i++) {
        uint64_t seq = 0;
        if (cmq_filestore_append(fs, payload, sizeof(payload), &seq) != 0) {
            ASSERT(0);
        }
        last_seq = seq;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (double)(t1.tv_sec - t0.tv_sec) +
                (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    double rate = (double)WAL_BENCH_N / dt;
    printf("  WAL appends: %d in %.3fs = %.0f append/s (last_seq=%llu)\n",
           WAL_BENCH_N, dt, rate, (unsigned long long)last_seq);
    /* Sanity: appends must finish within reasonable time. */
    ASSERT(dt < 30.0);
    ASSERT_EQ(last_seq, (uint64_t)WAL_BENCH_N);

    cmq_filestore_destroy(fs);
    int rc2 __attribute__((unused)) = system("rm -rf " WAL_BENCH_DIR);
    (void)rc2;
}

TEST_MAIN()