#include "cmq_cluster.h"
#include "cmq_route.h"
#include "cmq_gateway.h"
#include "cmq_leaf.h"
#include "cmq_atomic.h"
#include "cmq_test.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

TEST(cluster, create_destroy) {
    cmq_cluster_t *c = cmq_cluster_create("test-cluster", "node-1");
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(cmq_cluster_name(c), "test-cluster");
    ASSERT_STR_EQ(cmq_cluster_self_id(c), "node-1");
    cmq_cluster_destroy(c);
}

TEST(cluster, add_remove_nodes) {
    cmq_cluster_t *c = cmq_cluster_create("c1", "n1");
    ASSERT_EQ(cmq_cluster_add_node(c, "n2", "10.0.0.2", 7654), 0);
    ASSERT_EQ(cmq_cluster_add_node(c, "n3", "10.0.0.3", 7654), 0);
    ASSERT_EQ(cmq_cluster_node_count(c), (size_t)2);

    ASSERT_EQ(cmq_cluster_add_node(c, "n2", "10.0.0.2", 7654), 0);
    ASSERT_EQ(cmq_cluster_node_count(c), (size_t)2);

    cmq_node_info_t n;
    ASSERT_EQ(cmq_cluster_get_node(c, "n2", &n), 0);
    ASSERT_STR_EQ(n.addr, "10.0.0.2");
    ASSERT_EQ(n.port, 7654);

    ASSERT_EQ(cmq_cluster_remove_node(c, "n2"), 0);
    ASSERT_EQ(cmq_cluster_node_count(c), (size_t)1);
    ASSERT_EQ(cmq_cluster_get_node(c, "n2", &n), -1);

    cmq_cluster_destroy(c);
}

TEST(cluster, node_state) {
    cmq_cluster_t *c = cmq_cluster_create("c1", "n1");
    cmq_cluster_add_node(c, "n2", "10.0.0.2", 7654);

    cmq_node_info_t n;
    ASSERT_EQ(cmq_cluster_get_node(c, "n2", &n), 0);
    ASSERT_EQ(n.state, CMQ_NODE_JOINING);

    cmq_cluster_set_node_state(c, "n2", CMQ_NODE_ACTIVE);
    ASSERT_EQ(cmq_cluster_get_node(c, "n2", &n), 0);
    ASSERT_EQ(n.state, CMQ_NODE_ACTIVE);
    ASSERT_EQ(cmq_cluster_active_count(c), (size_t)1);

    cmq_cluster_destroy(c);
}

TEST(cluster, heartbeat) {
    cmq_cluster_t *c = cmq_cluster_create("c1", "n1");
    cmq_cluster_add_node(c, "n2", "10.0.0.2", 7654);

    int64_t elapsed = cmq_cluster_ms_since_heartbeat(c, "n2");
    ASSERT(elapsed >= 0);
    ASSERT(elapsed < 100);

    cmq_cluster_destroy(c);
}

TEST(cluster, list_nodes) {
    cmq_cluster_t *c = cmq_cluster_create("c1", "n1");
    cmq_cluster_add_node(c, "n2", "10.0.0.2", 7654);
    cmq_cluster_add_node(c, "n3", "10.0.0.3", 7654);

    cmq_node_info_t nodes[4];
    size_t n = cmq_cluster_list_nodes(c, nodes, 4);
    ASSERT_EQ(n, (size_t)2);
    ASSERT(strcmp(nodes[0].id, "n2") == 0 || strcmp(nodes[0].id, "n3") == 0);

    cmq_cluster_destroy(c);
}

TEST(route, create_destroy) {
    cmq_cluster_t *c = cmq_cluster_create("c1", "n1");
    cmq_route_pool_t *rp = cmq_route_pool_create(c);
    ASSERT_NOT_NULL(rp);
    ASSERT_EQ(cmq_route_pool_count(rp), (size_t)0);
    ASSERT_EQ(cmq_route_target_count(rp), (size_t)0);
    cmq_route_pool_destroy(rp);
    cmq_cluster_destroy(c);
}

/* dial_gate rejects attach/add/mark (align outbound connect abort on drain). */
TEST(route, dial_gate_blocks_inbound) {
    cmq_cluster_t *c = cmq_cluster_create("c1", "n1");
    cmq_route_pool_t *rp = cmq_route_pool_create(c);
    cmq_atomic_int gate;
    cmq_atomic_store_int(&gate, 1, CMQ_ATOMIC_RELEASE);
    cmq_route_pool_set_dial_gate(rp, &gate);
    int a[2];
    ASSERT_EQ(pipe(a), 0);
    ASSERT_EQ(cmq_route_attach_inbound(rp, "r0", a[1]), -1);
    ASSERT_EQ(cmq_route_add_conn(rp, "n2", -1, NULL, NULL), -1);
    ASSERT_EQ(write(a[1], "x", 1), 1); /* borrow not closed */
    close(a[0]);
    close(a[1]);
    cmq_route_pool_destroy(rp);
    cmq_cluster_destroy(c);
}

/* Failed dial still registers a target — forward_missed must see cluster intent. */
TEST(route, connect_fail_keeps_target) {
    cmq_cluster_t *c = cmq_cluster_create("c1", "n1");
    cmq_route_pool_t *rp = cmq_route_pool_create(c);
    ASSERT_NOT_NULL(rp);
    ASSERT_EQ(cmq_route_connect(rp, "n2", "127.0.0.1", 1, NULL, NULL), -1);
    ASSERT_EQ(cmq_route_live_count(rp), (size_t)0);
    ASSERT_EQ(cmq_route_pool_count(rp), (size_t)0);
    ASSERT_EQ(cmq_route_target_count(rp), (size_t)1);
    cmq_route_pool_destroy(rp);
    cmq_cluster_destroy(c);
}

TEST(route, add_remove_conn) {
    cmq_cluster_t *c = cmq_cluster_create("c1", "n1");
    cmq_route_pool_t *rp = cmq_route_pool_create(c);

    ASSERT_EQ(cmq_route_add_conn(rp, "n2", -1, NULL, NULL), 0);
    ASSERT_EQ(cmq_route_pool_count(rp), (size_t)1);

    ASSERT_EQ(cmq_route_add_conn(rp, "n2", -1, NULL, NULL), 0);
    ASSERT_EQ(cmq_route_pool_count(rp), (size_t)1);

    ASSERT_EQ(cmq_route_add_conn(rp, "n3", -1, NULL, NULL), 0);
    ASSERT_EQ(cmq_route_pool_count(rp), (size_t)2);

    cmq_route_conn_t conn;
    ASSERT_EQ(cmq_route_get_conn(rp, "n2", &conn), 0);
    ASSERT_STR_EQ(conn.remote_id, "n2");
    ASSERT_EQ(conn.connected, 1);

    ASSERT_EQ(cmq_route_disconnect(rp, "n2"), 0);
    ASSERT_EQ(cmq_route_pool_count(rp), (size_t)1);
    ASSERT_EQ(cmq_route_get_conn(rp, "n2", &conn), -1);

    ASSERT_EQ(cmq_route_disconnect(rp, "nonexistent"), -1);

    cmq_route_pool_destroy(rp);
    cmq_cluster_destroy(c);
}

/* Pool-full add_conn must close the caller-owned fd (no leak). */
TEST(route, add_conn_pool_full_closes_fd) {
    cmq_cluster_t *c = cmq_cluster_create("c1", "n1");
    cmq_route_pool_t *rp = cmq_route_pool_create(c);
    for (int i = 0; i < 32; i++) {
        char id[16];
        snprintf(id, sizeof(id), "n%d", i);
        ASSERT_EQ(cmq_route_add_conn(rp, id, -1, NULL, NULL), 0);
    }
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    ASSERT_EQ(cmq_route_add_conn(rp, "overflow", fds[1], NULL, NULL), -1);
    /* Write end should be closed by add_conn; write must fail with EBADF. */
    errno = 0;
    ASSERT(write(fds[1], "x", 1) < 0);
    ASSERT_EQ(errno, EBADF);
    close(fds[0]);
    cmq_route_pool_destroy(rp);
    cmq_cluster_destroy(c);
}

/* Inbound route fd is borrowed: destroy/detach must not close it. */
TEST(route, attach_inbound_borrow) {
    cmq_cluster_t *c = cmq_cluster_create("c1", "n1");
    cmq_route_pool_t *rp = cmq_route_pool_create(c);
    int a[2], b[2];
    ASSERT_EQ(pipe(a), 0);
    ASSERT_EQ(pipe(b), 0);

    ASSERT_EQ(cmq_route_attach_inbound(rp, "r0", a[1]), 0);
    ASSERT_EQ(cmq_route_live_count(rp), (size_t)0); /* staged until mark */
    ASSERT_EQ(cmq_route_held_count(rp), (size_t)1);
    ASSERT_EQ(cmq_route_peer_live(rp, "r0"), 1); /* staged blocks reconnect */
    /* add_conn must not SHUT_RDWR a staged inbound. */
    ASSERT_EQ(cmq_route_add_conn(rp, "r0", -1, NULL, NULL), 0);
    cmq_route_conn_t staged;
    ASSERT_EQ(cmq_route_get_conn(rp, "r0", &staged), 0);
    ASSERT_EQ(staged.fd, a[1]);
    ASSERT_EQ(staged.connected, 0);
    ASSERT_EQ(cmq_route_mark_connected(rp, a[1]), 0);
    ASSERT_EQ(cmq_route_live_count(rp), (size_t)1);
    cmq_route_conn_t conn;
    ASSERT_EQ(cmq_route_get_conn(rp, "r0", &conn), 0);
    ASSERT_EQ(conn.fd, a[1]);
    ASSERT_EQ(conn.fd_owned, 0);
    ASSERT_EQ(conn.connected, 1);

    /* CLOSING demote: not broadcast-live, but fd kept for flush/io_lock. */
    cmq_route_unmark_connected_fd(rp, a[1]);
    ASSERT_EQ(cmq_route_live_count(rp), (size_t)0);
    ASSERT_EQ(cmq_route_held_count(rp), (size_t)1);
    ASSERT_EQ(cmq_route_get_conn(rp, "r0", &conn), 0);
    ASSERT_EQ(conn.fd, a[1]);
    ASSERT_EQ(conn.connected, 0);
    ASSERT_EQ(cmq_route_peer_live(rp, "r0"), 1); /* held fd blocks reconnect */
    ASSERT_EQ(cmq_route_mark_connected(rp, a[1]), 0); /* restore for later */
    ASSERT_EQ(cmq_route_live_count(rp), (size_t)1);

    /* Live/staged egress already present — reject redundant inbound. */
    ASSERT_EQ(cmq_route_attach_inbound(rp, "r0", b[1]), -1);
    ASSERT_EQ(cmq_route_get_conn(rp, "r0", &conn), 0);
    ASSERT_EQ(conn.fd, a[1]);
    /* Rejected fd must still be usable by caller. */
    ASSERT_EQ(write(b[1], "y", 1), 1);

    cmq_route_detach_fd(rp, a[1]);
    ASSERT_EQ(cmq_route_live_count(rp), (size_t)0);

    /* Re-attach after detach, then destroy must not close borrowed fd. */
    ASSERT_EQ(cmq_route_attach_inbound(rp, "r0", a[1]), 0);
    cmq_route_pool_destroy(rp);
    errno = 0;
    ASSERT_EQ(write(a[1], "x", 1), 1);
    close(a[0]);
    close(a[1]);
    close(b[0]);
    close(b[1]);
    cmq_cluster_destroy(c);
}

/* Placeholder slots (fd=-1, connected=1) must not block reconnect. */
TEST(route, connect_replaces_placeholder) {
    cmq_cluster_t *c = cmq_cluster_create("c1", "n1");
    cmq_route_pool_t *rp = cmq_route_pool_create(c);
    ASSERT_EQ(cmq_route_add_conn(rp, "r0", -1, NULL, NULL), 0);
    cmq_route_conn_t conn;
    ASSERT_EQ(cmq_route_get_conn(rp, "r0", &conn), 0);
    ASSERT_EQ(conn.connected, 1);
    ASSERT_EQ(conn.fd, -1);
    ASSERT_EQ(cmq_route_live_count(rp), (size_t)0);
    /* Unreachable peer: must attempt connect (not early-return success). */
    ASSERT_EQ(cmq_route_connect(rp, "r0", "127.0.0.1", 1, NULL, NULL), -1);
    cmq_route_pool_destroy(rp);
    cmq_cluster_destroy(c);
}

TEST(gateway, create_destroy) {
    cmq_gateway_t *gw = cmq_gateway_create("local-cluster");
    ASSERT_NOT_NULL(gw);
    ASSERT_EQ(cmq_gateway_known_cluster_count(gw), (size_t)0);
    cmq_gateway_destroy(gw);
}

TEST(gateway, add_remote) {
    cmq_gateway_t *gw = cmq_gateway_create("local");
    ASSERT_EQ(cmq_gateway_add_remote(gw, "remote-1", "10.0.1.1", 7654), 0);
    ASSERT_EQ(cmq_gateway_known_cluster_count(gw), (size_t)1);

    cmq_gw_cluster_info_t ci;
    ASSERT_EQ(cmq_gateway_get_cluster(gw, "remote-1", &ci), 0);
    ASSERT_STR_EQ(ci.addr, "10.0.1.1");
    ASSERT_EQ(ci.port, 7654);

    ASSERT_EQ(cmq_gateway_add_remote(gw, "remote-1", "10.0.1.1", 7655), 0);
    ASSERT_EQ(cmq_gateway_known_cluster_count(gw), (size_t)1);
    ASSERT_EQ(cmq_gateway_get_cluster(gw, "remote-1", &ci), 0);
    ASSERT_EQ(ci.port, 7655);

    cmq_gateway_destroy(gw);
}

TEST(leaf, create_destroy) {
    cmq_leaf_node_t *l = cmq_leaf_create("10.0.0.1", 7654);
    ASSERT_NOT_NULL(l);
    char hub[CMQ_NODE_ADDR_SIZE];
    ASSERT_EQ(cmq_leaf_hub_addr(l, hub, sizeof(hub)), 0);
    ASSERT_STR_EQ(hub, "10.0.0.1");
    ASSERT_EQ(cmq_leaf_hub_port(l), 7654);
    ASSERT_EQ(cmq_leaf_is_connected(l), 0);
    ASSERT_EQ(cmq_leaf_poll(l), 0);
    cmq_leaf_destroy(l);
}

TEST(leaf, subscribe_unsubscribe) {
    cmq_leaf_node_t *l = cmq_leaf_create("10.0.0.1", 7654);
    ASSERT_EQ(cmq_leaf_subscribe(l, "foo.bar"), 0);
    ASSERT_EQ(cmq_leaf_subscribe(l, "foo.baz"), 0);
    ASSERT_EQ(cmq_leaf_sub_count(l), (size_t)2);

    ASSERT_EQ(cmq_leaf_subscribe(l, "foo.bar"), 0);
    ASSERT_EQ(cmq_leaf_sub_count(l), (size_t)2);

    ASSERT_EQ(cmq_leaf_unsubscribe(l, "foo.bar"), 0);
    ASSERT_EQ(cmq_leaf_sub_count(l), (size_t)1);

    cmq_leaf_destroy(l);
}

TEST(leaf, accept_remove) {
    cmq_leaf_node_t *l = cmq_leaf_create("10.0.0.1", 7654);
    ASSERT_EQ(cmq_leaf_accept(l, -1, "leaf-1"), 0);
    ASSERT_EQ(cmq_leaf_accept(l, -1, "leaf-2"), 0);
    ASSERT_EQ(cmq_leaf_accept_count(l), (size_t)2);

    ASSERT_EQ(cmq_leaf_remove(l, "leaf-1"), 0);
    ASSERT_EQ(cmq_leaf_accept_count(l), (size_t)1);

    cmq_leaf_destroy(l);
}

/* Pool-full accept must close the caller-owned fd. */
TEST(leaf, accept_pool_full_closes_fd) {
    cmq_leaf_node_t *l = cmq_leaf_create("10.0.0.1", 7654);
    for (int i = 0; i < 64; i++) {
        char id[16];
        snprintf(id, sizeof(id), "L%d", i);
        ASSERT_EQ(cmq_leaf_accept(l, -1, id), 0);
    }
    int overflow[2];
    ASSERT_EQ(pipe(overflow), 0);
    close(overflow[0]);
    ASSERT_EQ(cmq_leaf_accept(l, overflow[1], "overflow"), -1);
    errno = 0;
    ASSERT(write(overflow[1], "x", 1) < 0);
    ASSERT_EQ(errno, EBADF);
    cmq_leaf_destroy(l);
}

TEST_MAIN()
