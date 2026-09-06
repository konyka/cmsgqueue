#ifndef CMQ_TXN_H
#define CMQ_TXN_H

#include <stdint.h>
#include <stddef.h>

#define CMQ_TXN_MAGIC "CMQT"
#define CMQ_TXN_HDR_LEN 13
#define CMQ_TXN_BEGIN  1
#define CMQ_TXN_ADD    2
#define CMQ_TXN_COMMIT 3
#define CMQ_TXN_ABORT  4
#define CMQ_TXN_PREPARE 5
#define CMQ_TXN_VOTE    6
#define CMQ_TXN_MAX    32
#define CMQ_TXN_OPS    8
#define CMQ_TXN_PARTS  8
#define CMQ_TXN_SUB_MAX 128
#define CMQ_TXN_PAY_MAX 1024
#define CMQ_TXN_NODE_MAX 16
#define CMQ_TXN_VOTE_LEN 30
#define CMQ_TXN_2PC_MS 200

typedef struct cmq_txn cmq_txn_t;
typedef int (*cmq_txn_apply_fn)(void *ctx, const char *subject,
                                const uint8_t *data, size_t len);

cmq_txn_t *cmq_txn_create(void);
void cmq_txn_destroy(cmq_txn_t *t);

/* Opt-in durable log at dir/cmq.txn. Missing file is empty. */
int cmq_txn_set_log(cmq_txn_t *t, const char *dir);

int cmq_txn_encode(uint8_t *out, size_t cap, uint64_t id, uint8_t op,
                   size_t *out_len);
int cmq_txn_parse(const uint8_t *hdr, size_t n, uint64_t *id, uint8_t *op);

int cmq_txn_begin(cmq_txn_t *t, uint64_t id);
int cmq_txn_add(cmq_txn_t *t, uint64_t id, const char *subject,
                const uint8_t *data, size_t len);
int cmq_txn_commit(cmq_txn_t *t, uint64_t id, cmq_txn_apply_fn fn, void *ctx);
int cmq_txn_abort(cmq_txn_t *t, uint64_t id);
int cmq_txn_was_committed(cmq_txn_t *t, uint64_t id);
int cmq_txn_add_part(cmq_txn_t *t, uint64_t id, const char *node_id);
int cmq_txn_prepare(cmq_txn_t *t, uint64_t id);
int cmq_txn_vote(cmq_txn_t *t, uint64_t id, const char *node_id, int yes);
int cmq_txn_prepared(cmq_txn_t *t, uint64_t id);
int cmq_txn_all_yes(cmq_txn_t *t, uint64_t id);
int cmq_txn_part_count(cmq_txn_t *t, uint64_t id);
int cmq_txn_wait_votes(cmq_txn_t *t, uint64_t id, unsigned timeout_ms);
int cmq_txn_op_count(cmq_txn_t *t, uint64_t id);
int cmq_txn_op_at(cmq_txn_t *t, uint64_t id, int idx, char *subject,
                  size_t sub_cap, uint8_t *data, size_t data_cap,
                  size_t *dlen);
int cmq_txn_encode_vote(uint8_t *out, size_t cap, uint64_t id,
                        const char *node_id, int yes, size_t *out_len);
int cmq_txn_parse_vote(const uint8_t *hdr, size_t n, uint64_t *id,
                       char *node_id, size_t node_cap, int *yes);

#endif
