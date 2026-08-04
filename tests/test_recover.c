/* F5: WAL recovery on restart.
 *
 * Phase 1 — write a few records to a filestore.
 * Phase 2 — manually verify last_seq.
 * Phase 3 — open a NEW handle and read each record back.
 *
 * This is the building block for the cmq_server_create recovery
 * loop. End-to-end (server-create + restart) is tested manually
 * because the test framework cannot easily fork+restart a server.
 */

#include "cmq_test.h"
#include "cmq_filestore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RECOVER_DIR  "/tmp/cmq-test-recover"

TEST(recover, write_and_read_back) {
    system("rm -rf " RECOVER_DIR);

    /* Phase 1: write. */
    cmq_filestore_t *fs = cmq_filestore_create(RECOVER_DIR, "cmq");
    ASSERT_NOT_NULL(fs);
    uint64_t seq;

    const uint8_t rec1[] = {0x00, 0x05, 'h', 'e', 'l', 'l', 'o'};
    ASSERT_EQ(cmq_filestore_append(fs, rec1, sizeof(rec1), &seq), 0);
    ASSERT_EQ(seq, 1);

    const uint8_t rec2[] = {0x00, 0x05, 'w', 'o', 'r', 'l', 'd'};
    ASSERT_EQ(cmq_filestore_append(fs, rec2, sizeof(rec2), &seq), 0);
    ASSERT_EQ(seq, 2);

    uint64_t last = cmq_filestore_last_seq(fs);
    ASSERT_EQ(last, 2);
    cmq_filestore_sync(fs);
    cmq_filestore_destroy(fs);

    /* Phase 2: re-open and read every record. */
    cmq_filestore_t *fs2 = cmq_filestore_create(RECOVER_DIR, "cmq");
    ASSERT_NOT_NULL(fs2);
    ASSERT_EQ(cmq_filestore_last_seq(fs2), 2);

    /* Read record 1. */
    uint8_t *data = NULL;
    size_t data_len = 0;
    ASSERT_EQ(cmq_filestore_read(fs2, 1, &data, &data_len), 0);
    ASSERT_EQ(data_len, sizeof(rec1));
    ASSERT(memcmp(data, rec1, data_len) == 0);
    free(data);

    /* Read record 2. */
    data = NULL;
    data_len = 0;
    ASSERT_EQ(cmq_filestore_read(fs2, 2, &data, &data_len), 0);
    ASSERT_EQ(data_len, sizeof(rec2));
    ASSERT(memcmp(data, rec2, data_len) == 0);
    free(data);

    /* Read beyond end: error. */
    data = NULL;
    data_len = 0;
    int rc = cmq_filestore_read(fs2, 3, &data, &data_len);
    ASSERT(rc != 0);

    cmq_filestore_destroy(fs2);
    system("rm -rf " RECOVER_DIR);
}

TEST(recover, empty_filestore) {
    system("rm -rf /tmp/cmq-test-empty-recover");
    cmq_filestore_t *fs = cmq_filestore_create("/tmp/cmq-test-empty-recover", "cmq");
    ASSERT_NOT_NULL(fs);
    uint64_t last = cmq_filestore_last_seq(fs);
    ASSERT_EQ(last, 0);
    cmq_filestore_destroy(fs);
    system("rm -rf /tmp/cmq-test-empty-recover");
}

TEST_MAIN()
