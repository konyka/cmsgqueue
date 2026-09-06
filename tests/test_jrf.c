/* v0.5.131: reload applies jwks_refresh_sec. */
#include "cmq_jwksf.h"
#include "cmq_test.h"
#include <string.h>

TEST(jrf, apply) {
    cmq_jwks_url_t u;
    memset(&u, 0, sizeof(u));
    memcpy(u.host, "127.0.0.1", 10);
    memcpy(u.path, "/jwks", 6);
    u.port = 1;
    cmq_jwks_cache_t *c = cmq_jwks_cache_create();
    ASSERT(c != NULL);
    cmq_jwks_refresher_t *r = cmq_jwks_refresh_start(&u, c, 60);
    ASSERT(r != NULL);
    ASSERT_EQ(cmq_jwks_refresh_interval(r), 60u);
    int live = 60;
    ASSERT_EQ(cmq_jwks_refresh_reload(r, &live, 120), 0);
    ASSERT_EQ(live, 120);
    ASSERT_EQ(cmq_jwks_refresh_interval(r), 120u);
    cmq_jwks_refresh_stop(r);
    cmq_jwks_cache_destroy(c);
}

TEST(jrf, omitted) {
    int live = 30;
    ASSERT_EQ(cmq_jwks_refresh_reload(NULL, &live, 0), 0);
    ASSERT_EQ(live, 30);
}

TEST(jrf, empty) {
    int live = 45;
    ASSERT_EQ(cmq_jwks_refresh_reload(NULL, &live, 0), 0);
    ASSERT_EQ(live, 45);
}

TEST(jrf, reject) {
    int live = 60;
    ASSERT(cmq_jwks_refresh_reload(NULL, &live, 4) != 0);
    ASSERT_EQ(live, 60);
    ASSERT(cmq_jwks_refresh_reload(NULL, &live, 86401) != 0);
    ASSERT_EQ(live, 60);
    ASSERT(cmq_jwks_refresh_reload(NULL, NULL, 30) != 0);
}

TEST_MAIN()
