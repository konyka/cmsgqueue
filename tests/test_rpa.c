/* v0.5.162: reload attaches the route pool when create left it NULL. */
#include "cmq_cluster.h"
#include "cmq_route.h"
#include "cmq_test.h"

TEST(rpa, apply) {
    cmq_cluster_t *c = cmq_cluster_create("cl", "n1");
    ASSERT_NOT_NULL(c);
    cmq_route_pool_t *p = NULL;
    cmq_atomic_int gate;
    cmq_atomic_store_int(&gate, 0, CMQ_ATOMIC_RELAXED);
    ASSERT_EQ(cmq_route_pool_reload_attach(&p, c, &gate), 0);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cmq_route_pool_count(p), 0u);
    cmq_route_pool_t *same = p;
    ASSERT_EQ(cmq_route_pool_reload_attach(&p, c, &gate), 0);
    ASSERT(p == same);
    ASSERT_EQ(cmq_route_pool_count(p), 0u);
    cmq_route_pool_destroy(p);
    cmq_cluster_destroy(c);
}

TEST(rpa, omitted) {
    cmq_route_pool_t *p = NULL;
    ASSERT_EQ(cmq_route_pool_reload_attach(&p, NULL, NULL), 0);
    ASSERT(p == NULL);
}

TEST(rpa, empty) {
    cmq_cluster_t *c = cmq_cluster_create("cl", "n1");
    ASSERT_NOT_NULL(c);
    cmq_route_pool_t *p = cmq_route_pool_create(c);
    ASSERT_NOT_NULL(p);
    cmq_route_pool_t *same = p;
    ASSERT_EQ(cmq_route_pool_reload_attach(&p, NULL, NULL), 0);
    ASSERT(p == same);
    cmq_route_pool_destroy(p);
    cmq_cluster_destroy(c);
}

TEST(rpa, reject) {
    ASSERT(cmq_route_pool_reload_attach(NULL, NULL, NULL) != 0);
    cmq_cluster_t *c = cmq_cluster_create("cl", "n1");
    ASSERT_NOT_NULL(c);
    ASSERT(cmq_route_pool_reload_attach(NULL, c, NULL) != 0);
    cmq_cluster_destroy(c);
}

TEST_MAIN()
