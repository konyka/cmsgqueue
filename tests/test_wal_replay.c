/* P0: WAL replay must restore persisted messages without dropping them via
 * the account_epoch gate, and without re-appending (which would duplicate). */

#include "cmq_test.h"
#include "cmq_server.h"
#include "cmq_filestore.h"
#include "cmq_atomic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define REPLAY_DIR "/tmp/cmq-test-wal-replay"

TEST(persist_unit, replay_restores_messages) {
    system("rm -rf " REPLAY_DIR);

    cmq_filestore_t *fs = cmq_filestore_create(REPLAY_DIR, "cmq");
    ASSERT_NOT_NULL(fs);

    const char *subject = "replay.subject";
    uint16_t slen = (uint16_t)strlen(subject);
    uint8_t rec1[64];
    size_t off = 0;
    rec1[off++] = (uint8_t)(slen >> 8);
    rec1[off++] = (uint8_t)(slen & 0xFF);
    memcpy(rec1 + off, subject, slen);
    off += slen;
    rec1[off++] = 0; rec1[off++] = 0;
    const char *body = "hello";
    memcpy(rec1 + off, body, strlen(body));
    off += strlen(body);

    uint64_t seq = 0;
    ASSERT_EQ(cmq_filestore_append(fs, rec1, off, &seq), 0);
    ASSERT_EQ(seq, 1);
    ASSERT_EQ(cmq_filestore_append(fs, rec1, off, &seq), 0);
    ASSERT_EQ(seq, 2);
    ASSERT_EQ(cmq_filestore_append(fs, rec1, off, &seq), 0);
    ASSERT_EQ(seq, 3);
    cmq_filestore_sync(fs);
    cmq_filestore_destroy(fs);

    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19996;
    cfg.log_to_stdout = 0;
    cfg.persist_dir = REPLAY_DIR;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    uint64_t replayed = cmq_atomic_load_u64(&srv->stat_messages_replayed,
                                             CMQ_ATOMIC_RELAXED);
    printf("  stat_messages_replayed after restart = %llu (expect >= 3)\n",
           (unsigned long long)replayed);
    ASSERT(replayed >= 3);

    ASSERT_EQ(cmq_filestore_last_seq(srv->filestore), 3);

    cmq_server_destroy(srv);
    system("rm -rf " REPLAY_DIR);
}

TEST(persist_unit, replay_stats_separate_from_live) {
    /* P4: stat_messages_in reflects live traffic only. */
    system("rm -rf " REPLAY_DIR);

    cmq_filestore_t *fs = cmq_filestore_create(REPLAY_DIR, "cmq");
    ASSERT_NOT_NULL(fs);
    const char *subject = "replay.stats";
    uint16_t slen = (uint16_t)strlen(subject);
    uint8_t rec[64];
    size_t off = 0;
    rec[off++] = (uint8_t)(slen >> 8);
    rec[off++] = (uint8_t)(slen & 0xFF);
    memcpy(rec + off, subject, slen); off += slen;
    rec[off++] = 0; rec[off++] = 0;
    memcpy(rec + off, "hi", 2); off += 2;
    for (int i = 0; i < 5; i++) {
        uint64_t seq = 0;
        ASSERT_EQ(cmq_filestore_append(fs, rec, off, &seq), 0);
    }
    cmq_filestore_sync(fs);
    cmq_filestore_destroy(fs);

    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19985;
    cfg.log_to_stdout = 0;
    cfg.persist_dir = REPLAY_DIR;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    uint64_t replayed = cmq_atomic_load_u64(&srv->stat_messages_replayed,
                                             CMQ_ATOMIC_RELAXED);
    uint64_t in = cmq_atomic_load_u64(&srv->stat_messages_in,
                                       CMQ_ATOMIC_RELAXED);
    printf("  replayed=%llu in=%llu (expect replayed=5, in=0)\n",
           (unsigned long long)replayed, (unsigned long long)in);
    ASSERT_EQ(replayed, 5);
    ASSERT_EQ(in, 0);

    cmq_server_destroy(srv);
    system("rm -rf " REPLAY_DIR);
}

TEST(persist_unit, replay_handles_zero_records) {
    system("rm -rf /tmp/cmq-test-wal-replay-empty");
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19995;
    cfg.log_to_stdout = 0;
    cfg.persist_dir = "/tmp/cmq-test-wal-replay-empty";
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);
    ASSERT_EQ(cmq_filestore_last_seq(srv->filestore), 0);
    cmq_server_destroy(srv);
    system("rm -rf /tmp/cmq-test-wal-replay-empty");
}

TEST_MAIN()