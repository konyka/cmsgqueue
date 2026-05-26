#define _POSIX_C_SOURCE 200809L
#include "cmq_cluster.h"
#include "cmq_route.h"
#include "cmq_gateway.h"
#include "cmq_leaf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void demo_cluster(void) {
    printf("=== Cluster Membership Demo ===\n");
    cmq_cluster_t *cluster = cmq_cluster_create("dc1", "node-1");
    printf("Created cluster '%s', self_id='%s'\n",
           cmq_cluster_name(cluster), cmq_cluster_self_id(cluster));

    cmq_cluster_add_node(cluster, "node-2", "10.0.0.2", 7654);
    cmq_cluster_add_node(cluster, "node-3", "10.0.0.3", 7654);
    cmq_cluster_add_node(cluster, "node-4", "10.0.0.4", 7654);
    printf("Added 3 nodes. Total: %zu nodes\n", cmq_cluster_node_count(cluster));

    cmq_cluster_set_node_state(cluster, "node-2", CMQ_NODE_ACTIVE);
    cmq_cluster_set_node_state(cluster, "node-3", CMQ_NODE_ACTIVE);
    printf("Active nodes: %zu\n", cmq_cluster_active_count(cluster));

    cmq_node_info_t nodes[8];
    cmq_cluster_list_nodes(cluster, nodes, 8);
    for (size_t i = 0; i < cmq_cluster_node_count(cluster); i++) {
        printf("  %s @ %s:%d state=%d\n", nodes[i].id, nodes[i].addr, nodes[i].port, nodes[i].state);
    }

    cmq_cluster_destroy(cluster);
    printf("\n");
}

static void demo_routing(void) {
    printf("=== Route Connections Demo ===\n");
    cmq_cluster_t *cluster = cmq_cluster_create("dc1", "node-1");
    cmq_route_pool_t *routes = cmq_route_pool_create(cluster);

    cmq_route_add_conn(routes, "node-2", -1);
    cmq_route_add_conn(routes, "node-3", -1);
    printf("Added 2 route connections. Pool size: %zu\n", cmq_route_pool_count(routes));

    cmq_route_conn_t *c = cmq_route_get_conn(routes, "node-2");
    if (c) printf("Route to node-2: connected=%d\n", c->connected);

    cmq_route_disconnect(routes, "node-2");
    printf("After disconnect node-2: pool size=%zu\n", cmq_route_pool_count(routes));

    cmq_route_pool_destroy(routes);
    cmq_cluster_destroy(cluster);
    printf("\n");
}

static void demo_gateway(void) {
    printf("=== Gateway (Cross-Cluster) Demo ===\n");
    cmq_gateway_t *gw = cmq_gateway_create("dc1");

    cmq_gateway_add_remote(gw, "dc2", "10.1.0.1", 7654);
    cmq_gateway_add_remote(gw, "dc3", "10.2.0.1", 7654);
    printf("Registered %zu remote clusters\n", cmq_gateway_known_cluster_count(gw));

    cmq_gw_cluster_info_t *ci = cmq_gateway_get_cluster(gw, "dc2");
    if (ci) printf("  dc2 @ %s:%d\n", ci->addr, ci->port);

    cmq_gateway_destroy(gw);
    printf("\n");
}

static void demo_leaf(void) {
    printf("=== Leaf Node Demo ===\n");
    cmq_leaf_node_t *leaf = cmq_leaf_create("10.0.0.1", 7654);
    printf("Leaf hub: %s:%d, connected=%d\n",
           cmq_leaf_hub_addr(leaf), cmq_leaf_hub_port(leaf),
           cmq_leaf_is_connected(leaf));

    cmq_leaf_subscribe(leaf, "sensors.temperature");
    cmq_leaf_subscribe(leaf, "sensors.humidity");
    cmq_leaf_subscribe(leaf, "alerts.>");
    printf("Subscribed to %zu subjects\n", cmq_leaf_sub_count(leaf));

    cmq_leaf_accept(leaf, -1, "edge-device-1");
    cmq_leaf_accept(leaf, -1, "edge-device-2");
    printf("Accepted %zu leaf connections\n", cmq_leaf_accept_count(leaf));

    cmq_leaf_remove(leaf, "edge-device-1");
    printf("After removing edge-device-1: %zu leaves\n", cmq_leaf_accept_count(leaf));

    cmq_leaf_destroy(leaf);
    printf("\n");
}

int main(void) {
    printf("CMSGQueue Clustering Demo\n");
    printf("=========================\n\n");
    demo_cluster();
    demo_routing();
    demo_gateway();
    demo_leaf();
    printf("All clustering demos complete.\n");
    return 0;
}
