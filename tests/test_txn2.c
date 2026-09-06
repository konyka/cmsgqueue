/* v0.5.80: D5 multi-node 2PC prepare/vote. */
#include "cmq_test.h"
#include "cmq_txn.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    int n;
    char sub[8][128];
} apply_sink_t;

static int sink_apply(void *ctx, const char *subject,
                      const uint8_t *data, size_t len) {
    apply_sink_t *s = ctx;
    (void)data;
    (void)len;
    if (!s || s->n >= 8) return -1;
    snprintf(s->sub[s->n], sizeof(s->sub[0]), "%s", subject);
    s->n++;
    return 0;
}

TEST(txn2, prepare_votes_commit) {
    cmq_txn_t *t = cmq_txn_create();
    ASSERT_NOT_NULL(t);
    ASSERT_EQ(cmq_txn_begin(t, 11), 0);
    ASSERT_EQ(cmq_txn_add(t, 11, "a.b", (const uint8_t *)"x", 1), 0);
    ASSERT_EQ(cmq_txn_add_part(t, 11, "n1"), 0);
    ASSERT_EQ(cmq_txn_add_part(t, 11, "n2"), 0);
    ASSERT_EQ(cmq_txn_part_count(t, 11), 2);
    ASSERT_EQ(cmq_txn_prepare(t, 11), 0);
    ASSERT_EQ(cmq_txn_prepared(t, 11), 1);
    apply_sink_t s;
    memset(&s, 0, sizeof(s));
    ASSERT(cmq_txn_commit(t, 11, sink_apply, &s) != 0);
    ASSERT_EQ(s.n, 0);
    ASSERT_EQ(cmq_txn_vote(t, 11, "n1", 1), 0);
    ASSERT_EQ(cmq_txn_vote(t, 11, "n2", 1), 0);
    ASSERT_EQ(cmq_txn_all_yes(t, 11), 1);
    ASSERT_EQ(cmq_txn_commit(t, 11, sink_apply, &s), 0);
    ASSERT_EQ(s.n, 1);
    ASSERT_STR_EQ(s.sub[0], "a.b");
    cmq_txn_destroy(t);
}

TEST(txn2, vote_no_fails) {
    cmq_txn_t *t = cmq_txn_create();
    ASSERT_EQ(cmq_txn_begin(t, 12), 0);
    ASSERT_EQ(cmq_txn_add(t, 12, "z", (const uint8_t *)"q", 1), 0);
    ASSERT_EQ(cmq_txn_add_part(t, 12, "n1"), 0);
    ASSERT_EQ(cmq_txn_prepare(t, 12), 0);
    ASSERT_EQ(cmq_txn_vote(t, 12, "n1", 0), 0);
    apply_sink_t s;
    memset(&s, 0, sizeof(s));
    ASSERT(cmq_txn_commit(t, 12, sink_apply, &s) != 0);
    ASSERT_EQ(s.n, 0);
    ASSERT_EQ(cmq_txn_was_committed(t, 12), 0);
    cmq_txn_destroy(t);
}

TEST(txn2, wire_prepare_vote) {
    uint8_t buf[32];
    size_t n = 0;
    ASSERT_EQ(cmq_txn_encode(buf, sizeof(buf), 7, CMQ_TXN_PREPARE, &n), 0);
    ASSERT_EQ(n, (size_t)CMQ_TXN_HDR_LEN);
    uint64_t id = 0;
    uint8_t op = 0;
    ASSERT_EQ(cmq_txn_parse(buf, n, &id, &op), 0);
    ASSERT_EQ(id, (uint64_t)7);
    ASSERT_EQ(op, (uint8_t)CMQ_TXN_PREPARE);
    ASSERT_EQ(cmq_txn_encode_vote(buf, sizeof(buf), 7, "node.a", 1, &n), 0);
    ASSERT_EQ(n, (size_t)CMQ_TXN_VOTE_LEN);
    char node[16];
    int yes = 0;
    ASSERT_EQ(cmq_txn_parse_vote(buf, n, &id, node, sizeof(node), &yes), 0);
    ASSERT_STR_EQ(node, "node.a");
    ASSERT_EQ(yes, 1);
}

TEST(txn2, reject) {
    cmq_txn_t *t = cmq_txn_create();
    ASSERT(cmq_txn_add_part(t, 1, "n1") != 0);
    ASSERT_EQ(cmq_txn_begin(t, 1), 0);
    ASSERT(cmq_txn_add_part(t, 1, "bad name") != 0);
    ASSERT(cmq_txn_vote(t, 1, "nope", 1) != 0);
    ASSERT(cmq_txn_prepare(t, 99) != 0);
    ASSERT(cmq_txn_encode_vote(NULL, 0, 1, "n", 1, NULL) != 0);
    ASSERT(cmq_txn_parse_vote((const uint8_t *)"CMQT", 4, NULL, NULL, 0,
                              NULL) != 0);
    cmq_txn_destroy(t);
}

TEST_MAIN()
