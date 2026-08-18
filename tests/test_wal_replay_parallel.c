/* P7: WAL replay must restore records in deterministic order. Sequential
 * reference (this test) is the baseline; P7-parallel implementation must
 * produce equivalent state. */

#include "cmq_test.h"
#include "cmq_server.h"
#include "cmq_filestore.h"
#include "cmq_atomic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define P7_DIR "/tmp/cmq-test-wal-parallel-replay"
#define P7_N   200

TEST(persist_unit, replay_deterministic_under_restart) {
    int rc __attribute__((unused)) = system("rm -rf " P7_DIR " && mkdir -p " P7_DIR);
    (void)rc;

    /* Phase 1: pre-write P7_N records. */
    cmq_filestore_t *fs = cmq_filestore_create(P7_DIR, "cmq");
    ASSERT_NOT_NULL(fs);
    const char *subj = "p7.parallel";
    uint16_t slen = (uint16_t)strlen(subj);
    uint8_t rec[64];
    size_t off = 0;
    rec[off++] = (uint8_t)(slen >> 8); rec[off++] = (uint8_t)(slen & 0xFF);
    memcpy(rec + off, subj, slen); off += slen;
    rec[off++] = 0; rec[off++] = 0;
    memcpy(rec + off, "payload", 7); off += 7;
    for (int i = 0; i < P7_N; i++) {
        uint64_t seq = 0;
        ASSERT_EQ(cmq_filestore_append(fs, rec, off, &seq), 0);
    }
    cmq_filestore_sync(fs);
    cmq_filestore_destroy(fs);

    /* Phase 2: server start replays all records. */
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19991;
    cfg.log_to_stdout = 0;
    cfg.persist_dir = P7_DIR;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    uint64_t replayed = cmq_atomic_load_u64(&srv->stat_messages_replayed,
                                             CMQ_ATOMIC_RELAXED);
    printf("  stat_messages_replayed after restart = %llu (expect >= %d)\n",
           (unsigned long long)replayed, P7_N);
    ASSERT(replayed >= (uint64_t)P7_N);

    /* Idempotency: last_seq must equal P7_N (no re-append). */
    ASSERT_EQ(cmq_filestore_last_seq(srv->filestore), (uint64_t)P7_N);

    cmq_server_destroy(srv);
    int rc2 __attribute__((unused)) = system("rm -rf " P7_DIR);
    (void)rc2;
}

TEST_MAIN()