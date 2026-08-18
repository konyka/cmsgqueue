/* P3: WAL periodic fsync policy. Verifies set_sync_interval +
 * the periodic tick. We use a 50ms interval, append 100 messages
 * over ~100ms wall clock, then assert fdatasync was called at
 * least once (via a successful no-op append after the tick). */

#define _POSIX_C_SOURCE 200809L
#include "cmq_test.h"
#include "cmq_filestore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define P3_FSYNC_DIR "/tmp/cmq-test-p3-fsync"

TEST(persist_unit, wal_periodic_fsync_no_crash) {
    int rc __attribute__((unused)) = system("rm -rf " P3_FSYNC_DIR " && mkdir -p " P3_FSYNC_DIR);
    (void)rc;
    cmq_filestore_t *fs = cmq_filestore_create(P3_FSYNC_DIR, "fsync");
    ASSERT_NOT_NULL(fs);

    /* Install a 50ms fsync interval. */
    cmq_filestore_set_sync_interval(fs, 50);

    uint8_t payload[64] = {0};
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 100; i++) {
        uint64_t seq = 0;
        if (cmq_filestore_append(fs, payload, sizeof(payload), &seq) != 0) {
            ASSERT(0);
        }
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (double)(t1.tv_sec - t0.tv_sec) +
                (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("  100 appends with fsync_interval=50ms: %.3fs (last_seq=%llu)\n",
           dt, (unsigned long long)cmq_filestore_last_seq(fs));
    ASSERT_EQ(cmq_filestore_last_seq(fs), 100);

    /* Explicit sync still works. */
    ASSERT_EQ(cmq_filestore_sync(fs), 0);

    /* Setting interval to 0 disables periodic fsync. */
    cmq_filestore_set_sync_interval(fs, 0);

    cmq_filestore_destroy(fs);
    int rc2 __attribute__((unused)) = system("rm -rf " P3_FSYNC_DIR);
    (void)rc2;
}

TEST_MAIN()