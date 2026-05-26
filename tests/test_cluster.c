#include "cmq_cluster.h"
#include "cmq_route.h"
#include "cmq_gateway.h"
#include "cmq_leaf.h"
#include "cmq_test.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

    cmq_node_info_t *n = cmq_cluster_get_node(c, "n2");
    ASSERT_NOT_NULL(n);
    ASSERT_STR_EQ(n->addr, "10.0.0.2");
    ASSERT_EQ(n->port, 7654);

    ASSERT_EQ(cmq_cluster_remove_node(c, "n2"), 0);
    ASSERT_EQ(cmq_cluster_node_count(c), (size_t)1);
    ASSERT_NULL(cmq_cluster_get_node(c, "n2"));

    cmq_cluster_destroy(c);
}

TEST(cluster, node_state) {
    cmq_cluster_t *c = cmq_cluster_create("c1", "n1");
    cmq_cluster_add_node(c, "n2", "10.0.0.2", 7654);

    cmq_node_info_t *n = cmq_cluster_get_node(c, "n2");
    ASSERT_EQ(n->state, CMQ_NODE_JOINING);

    cmq_cluster_set_node_state(c, "n2", CMQ_NODE_ACTIVE);
    ASSERT_EQ(n->state, CMQ_NODE_ACTIVE);
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
    cmq_cluster_list_nodes(c, nodes, 4);
    ASSERT(strcmp(nodes[0].id, "n2") == 0 || strcmp(nodes[0].id, "n3") == 0);

    cmq_cluster_destroy(c);
}

TEST(route, create_destroy) {
    cmq_cluster_t *c = cmq_cluster_create("c1", "n1");
    cmq_route_pool_t *rp = cmq_route_pool_create(c);
    ASSERT_NOT_NULL(rp);
    ASSERT_EQ(cmq_route_pool_count(rp), (size_t)0);
    cmq_route_pool_destroy(rp);
    cmq_cluster_destroy(c);
}

TEST(route, add_remove_conn) {
    cmq_cluster_t *c = cmq_cluster_create("c1", "n1");
    cmq_route_pool_t *rp = cmq_route_pool_create(c);

    ASSERT_EQ(cmq_route_add_conn(rp, "n2", -1), 0);
    ASSERT_EQ(cmq_route_pool_count(rp), (size_t)1);

    ASSERT_EQ(cmq_route_add_conn(rp, "n2", -1), 0);
    ASSERT_EQ(cmq_route_pool_count(rp), (size_t)1);

    ASSERT_EQ(cmq_route_add_conn(rp, "n3", -1), 0);
    ASSERT_EQ(cmq_route_pool_count(rp), (size_t)2);

    cmq_route_conn_t *conn = cmq_route_get_conn(rp, "n2");
    ASSERT_NOT_NULL(conn);
    ASSERT_STR_EQ(conn->remote_id, "n2");
    ASSERT_EQ(conn->connected, 1);

    ASSERT_EQ(cmq_route_disconnect(rp, "n2"), 0);
    ASSERT_EQ(cmq_route_pool_count(rp), (size_t)1);
    ASSERT_NULL(cmq_route_get_conn(rp, "n2"));

    ASSERT_EQ(cmq_route_disconnect(rp, "nonexistent"), -1);

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

    cmq_gw_cluster_info_t *ci = cmq_gateway_get_cluster(gw, "remote-1");
    ASSERT_NOT_NULL(ci);
    ASSERT_STR_EQ(ci->addr, "10.0.1.1");
    ASSERT_EQ(ci->port, 7654);

    ASSERT_EQ(cmq_gateway_add_remote(gw, "remote-1", "10.0.1.1", 7655), 0);
    ASSERT_EQ(cmq_gateway_known_cluster_count(gw), (size_t)1);
    ASSERT_EQ(ci->port, 7655);

    cmq_gateway_destroy(gw);
}

TEST(leaf, create_destroy) {
    cmq_leaf_node_t *l = cmq_leaf_create("10.0.0.1", 7654);
    ASSERT_NOT_NULL(l);
    ASSERT_STR_EQ(cmq_leaf_hub_addr(l), "10.0.0.1");
    ASSERT_EQ(cmq_leaf_hub_port(l), 7654);
    ASSERT_EQ(cmq_leaf_is_connected(l), 0);
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
    ASSERT_EQ(cmq_leaf_accept(l, 10, "leaf-1"), 0);
    ASSERT_EQ(cmq_leaf_accept(l, 11, "leaf-2"), 0);
    ASSERT_EQ(cmq_leaf_accept_count(l), (size_t)2);

    ASSERT_EQ(cmq_leaf_remove(l, "leaf-1"), 0);
    ASSERT_EQ(cmq_leaf_accept_count(l), (size_t)1);

    cmq_leaf_destroy(l);
}

TEST_MAIN()
