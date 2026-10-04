/* v0.5.85: D5 deferred route write retry. */
#include "cmq_test.h"
#include "cmq_cluster.h"
#include "cmq_route.h"

#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int pair_live(cmq_route_pool_t *rp, const char *id, int *peer) {
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return -1;
    if (cmq_route_attach_inbound(rp, id, sp[0]) != 0 ||
        cmq_route_mark_connected(rp, sp[0]) != 0) {
        close(sp[0]);
        close(sp[1]);
        return -1;
    }
    *peer = sp[1];
    return 0;
}

TEST(rtry, offer_drain) {
    cmq_cluster_t *c = cmq_cluster_create("c", "n1");
    ASSERT_NOT_NULL(c);
    cmq_route_pool_t *rp = cmq_route_pool_create(c);
    ASSERT_NOT_NULL(rp);
    int peer = -1;
    ASSERT_EQ(pair_live(rp, "n2", &peer), 0);
    const uint8_t msg[] = "hello";
    ASSERT_EQ(cmq_route_retry_offer(rp, "n2", msg, sizeof(msg)), 0);
    ASSERT_EQ(cmq_route_retry_pending(rp), 1u);
    ASSERT_EQ(cmq_route_retry_drain(rp), 1);
    ASSERT_EQ(cmq_route_retry_pending(rp), 0u);
    ASSERT_EQ(cmq_route_retry_sent(rp), 1u);
    uint8_t got[8];
    ASSERT_EQ((int)recv(peer, got, sizeof(msg), 0), (int)sizeof(msg));
    ASSERT(memcmp(got, msg, sizeof(msg)) == 0);
    close(peer);
    cmq_route_pool_destroy(rp);
    cmq_cluster_destroy(c);
}

TEST(rtry, full_drop) {
    cmq_cluster_t *c = cmq_cluster_create("c", "n1");
    cmq_route_pool_t *rp = cmq_route_pool_create(c);
    const uint8_t msg[] = "x";
    for (int i = 0; i < CMQ_ROUTE_RETRY_MAX; i++)
        ASSERT_EQ(cmq_route_retry_offer(rp, "n2", msg, 1), 0);
    ASSERT_EQ(cmq_route_retry_offer(rp, "n2", msg, 1), 1);
    ASSERT_EQ(cmq_route_retry_pending(rp), (size_t)CMQ_ROUTE_RETRY_MAX);
    ASSERT_EQ(cmq_route_retry_dropped(rp), 1u);
    cmq_route_pool_destroy(rp);
    cmq_cluster_destroy(c);
}

TEST(rtry, dead_skip) {
    cmq_cluster_t *c = cmq_cluster_create("c", "n1");
    cmq_route_pool_t *rp = cmq_route_pool_create(c);
    const uint8_t msg[] = "z";
    ASSERT_EQ(cmq_route_retry_offer(rp, "gone", msg, 1), 0);
    ASSERT_EQ(cmq_route_retry_drain(rp), 0);
    ASSERT_EQ(cmq_route_retry_pending(rp), 0u);
    ASSERT_EQ(cmq_route_retry_sent(rp), 0u);
    cmq_route_pool_destroy(rp);
    cmq_cluster_destroy(c);
}

TEST(rtry, reject) {
    ASSERT(cmq_route_retry_offer(NULL, "n", (const uint8_t *)"a", 1) != 0);
    ASSERT(cmq_route_retry_drain(NULL) < 0);
    ASSERT_EQ(cmq_route_retry_pending(NULL), 0u);
    uint8_t big[CMQ_ROUTE_RETRY_BYTES + 1];
    memset(big, 1, sizeof(big));
    cmq_cluster_t *c = cmq_cluster_create("c", "n1");
    cmq_route_pool_t *rp = cmq_route_pool_create(c);
    ASSERT(cmq_route_retry_offer(rp, "n2", big, sizeof(big)) != 0);
    cmq_route_pool_destroy(rp);
    cmq_cluster_destroy(c);
}

TEST_MAIN()
