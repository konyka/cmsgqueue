/* P1: async WAL ring smoke test. Verifies enqueue + worker drain. */

#define _POSIX_C_SOURCE 200809L
#include "cmq_test.h"
#include "cmq_filestore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define P1_ASYNC_DIR "/tmp/cmq-test-p1-async"

TEST(persist_unit, wal_async_enqueue_drains) {
    int rc __attribute__((unused)) = system("rm -rf " P1_ASYNC_DIR " && mkdir -p " P1_ASYNC_DIR);
    (void)rc;
    cmq_filestore_t *fs = cmq_filestore_create(P1_ASYNC_DIR, "async");
    ASSERT_NOT_NULL(fs);

    /* Enable async ring with capacity 16. */
    ASSERT_EQ(cmq_filestore_set_async(fs, 16), 0);

    /* Enqueue 8 entries; the worker drains them async. */
    uint8_t payload[64];
    memset(payload, 0xAB, sizeof(payload));
    for (uint64_t i = 1; i <= 8; i++) {
        if (cmq_filestore_async_enqueue(fs, payload, sizeof(payload), i) != 0) {
            ASSERT(0);
        }
    }

    /* Give the worker time to drain. */
    struct timespec ts = {0, 200000000};
    nanosleep(&ts, NULL);

    /* Final sync ensures any pending writes hit disk. */
    ASSERT_EQ(cmq_filestore_sync(fs), 0);

    cmq_filestore_destroy(fs);
    int rc2 __attribute__((unused)) = system("rm -rf " P1_ASYNC_DIR);
    (void)rc2;
}

TEST(persist_unit, wal_async_disabled_returns_error) {
    /* Without set_async, enqueue returns -1. */
    int rc __attribute__((unused)) = system("rm -rf /tmp/cmq-test-p1-noasync");
    (void)rc;
    cmq_filestore_t *fs = cmq_filestore_create("/tmp/cmq-test-p1-noasync", "noasync");
    ASSERT_NOT_NULL(fs);
    uint8_t buf[16] = {0};
    ASSERT_EQ(cmq_filestore_async_enqueue(fs, buf, sizeof(buf), 1), -1);
    cmq_filestore_destroy(fs);
}

TEST_MAIN()