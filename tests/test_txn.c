/* v0.5.60: D5 phase 2 transaction coordinator. */
#include "cmq_test.h"
#include "cmq_txn.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define TDIR "/tmp/cmq_txn"

static void wipe(void) {
    remove(TDIR "/cmq.txn");
    remove(TDIR "/cmq.txn.tmp");
}

typedef struct {
    int n;
    char sub[8][128];
    uint8_t pay[8][32];
    size_t len[8];
} apply_sink_t;

static int sink_apply(void *ctx, const char *subject, const uint8_t *data,
                      size_t len) {
    apply_sink_t *s = ctx;
    if (!s || s->n >= 8) return -1;
    snprintf(s->sub[s->n], sizeof(s->sub[0]), "%s", subject);
    if (len > 0)
        memcpy(s->pay[s->n], data, len < 32 ? len : 32);
    s->len[s->n] = len;
    s->n++;
    return 0;
}

TEST(txn, parse_roundtrip) {
    uint8_t buf[16];
    size_t n = 0;
    ASSERT_EQ(cmq_txn_encode(buf, sizeof(buf), 9, CMQ_TXN_COMMIT, &n), 0);
    ASSERT_EQ(n, (size_t)CMQ_TXN_HDR_LEN);
    uint64_t id = 0;
    uint8_t op = 0;
    ASSERT_EQ(cmq_txn_parse(buf, n, &id, &op), 0);
    ASSERT_EQ(id, (uint64_t)9);
    ASSERT_EQ(op, (uint8_t)CMQ_TXN_COMMIT);
    ASSERT_EQ(cmq_txn_encode(buf, sizeof(buf), 0, 1, &n), -1);
    ASSERT_EQ(cmq_txn_parse((const uint8_t *)"XXXX.........", 13, &id, &op),
              -1);
}

TEST(txn, commit_applies) {
    cmq_txn_t *t = cmq_txn_create();
    ASSERT_NOT_NULL(t);
    ASSERT_EQ(cmq_txn_begin(t, 1), 0);
    ASSERT_EQ(cmq_txn_add(t, 1, "a.b", (const uint8_t *)"x", 1), 0);
    ASSERT_EQ(cmq_txn_add(t, 1, "a.c", (const uint8_t *)"yz", 2), 0);
    apply_sink_t s;
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(cmq_txn_commit(t, 1, sink_apply, &s), 0);
    ASSERT_EQ(s.n, 2);
    ASSERT_STR_EQ(s.sub[0], "a.b");
    ASSERT_EQ(s.len[1], (size_t)2);
    ASSERT_EQ(cmq_txn_was_committed(t, 1), 1);
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(cmq_txn_commit(t, 1, sink_apply, &s), 0);
    ASSERT_EQ(s.n, 0);
    cmq_txn_destroy(t);
}

TEST(txn, abort_not_applied) {
    cmq_txn_t *t = cmq_txn_create();
    ASSERT_EQ(cmq_txn_begin(t, 2), 0);
    ASSERT_EQ(cmq_txn_add(t, 2, "z", (const uint8_t *)"q", 1), 0);
    ASSERT_EQ(cmq_txn_abort(t, 2), 0);
    apply_sink_t s;
    memset(&s, 0, sizeof(s));
    ASSERT(cmq_txn_commit(t, 2, sink_apply, &s) != 0);
    ASSERT_EQ(s.n, 0);
    ASSERT_EQ(cmq_txn_was_committed(t, 2), 0);
    cmq_txn_destroy(t);
}

TEST(txn, isolated) {
    cmq_txn_t *t = cmq_txn_create();
    ASSERT_EQ(cmq_txn_begin(t, 3), 0);
    ASSERT_EQ(cmq_txn_begin(t, 4), 0);
    ASSERT_EQ(cmq_txn_add(t, 3, "one", (const uint8_t *)"1", 1), 0);
    ASSERT_EQ(cmq_txn_add(t, 4, "two", (const uint8_t *)"2", 1), 0);
    ASSERT_EQ(cmq_txn_abort(t, 3), 0);
    apply_sink_t s;
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(cmq_txn_commit(t, 4, sink_apply, &s), 0);
    ASSERT_EQ(s.n, 1);
    ASSERT_STR_EQ(s.sub[0], "two");
    cmq_txn_destroy(t);
}

TEST(txn, no_log_no_file) {
    mkdir(TDIR, 0755);
    wipe();
    cmq_txn_t *t = cmq_txn_create();
    ASSERT_EQ(cmq_txn_begin(t, 5), 0);
    ASSERT_EQ(cmq_txn_commit(t, 5, NULL, NULL), 0);
    cmq_txn_destroy(t);
    struct stat st;
    ASSERT(stat(TDIR "/cmq.txn", &st) != 0);
}

TEST(txn, reopen_committed) {
    mkdir(TDIR, 0755);
    wipe();
    {
        cmq_txn_t *t = cmq_txn_create();
        ASSERT_EQ(cmq_txn_set_log(t, TDIR), 0);
        ASSERT_EQ(cmq_txn_begin(t, 7), 0);
        ASSERT_EQ(cmq_txn_add(t, 7, "k", (const uint8_t *)"v", 1), 0);
        apply_sink_t s;
        memset(&s, 0, sizeof(s));
        ASSERT_EQ(cmq_txn_commit(t, 7, sink_apply, &s), 0);
        cmq_txn_destroy(t);
    }
    {
        cmq_txn_t *t = cmq_txn_create();
        ASSERT_EQ(cmq_txn_set_log(t, TDIR), 0);
        ASSERT_EQ(cmq_txn_was_committed(t, 7), 1);
        apply_sink_t s;
        memset(&s, 0, sizeof(s));
        ASSERT_EQ(cmq_txn_commit(t, 7, sink_apply, &s), 0);
        ASSERT_EQ(s.n, 0);
        cmq_txn_destroy(t);
    }
    wipe();
}

TEST(txn, uncommitted_gone) {
    mkdir(TDIR, 0755);
    wipe();
    {
        cmq_txn_t *t = cmq_txn_create();
        ASSERT_EQ(cmq_txn_set_log(t, TDIR), 0);
        ASSERT_EQ(cmq_txn_begin(t, 8), 0);
        ASSERT_EQ(cmq_txn_add(t, 8, "k", (const uint8_t *)"v", 1), 0);
        cmq_txn_destroy(t);
    }
    {
        cmq_txn_t *t = cmq_txn_create();
        ASSERT_EQ(cmq_txn_set_log(t, TDIR), 0);
        ASSERT_EQ(cmq_txn_was_committed(t, 8), 0);
        apply_sink_t s;
        memset(&s, 0, sizeof(s));
        ASSERT(cmq_txn_commit(t, 8, sink_apply, &s) != 0);
        cmq_txn_destroy(t);
    }
    wipe();
}

TEST(txn, reject_full_and_bad) {
    cmq_txn_t *t = cmq_txn_create();
    ASSERT(cmq_txn_begin(t, 0) != 0);
    ASSERT(cmq_txn_set_log(t, "/tmp/../etc") != 0);
    for (uint64_t i = 1; i <= (uint64_t)CMQ_TXN_MAX; i++)
        ASSERT_EQ(cmq_txn_begin(t, i), 0);
    ASSERT_EQ(cmq_txn_begin(t, 99), -2);
    ASSERT(cmq_txn_add(t, 1, "bad name", (const uint8_t *)"x", 1) != 0);
    cmq_txn_destroy(t);
}

TEST_MAIN()
