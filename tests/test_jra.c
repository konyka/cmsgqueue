/* v0.5.141: reload starts JWKS refresh when create had none. */
#include "cmq_jwksf.h"
#include "cmq_test.h"
#include <string.h>

TEST(jra, apply) {
    cmq_jwks_cache_t *c = cmq_jwks_cache_create();
    ASSERT(c != NULL);
    cmq_jwks_refresher_t *r = NULL;
    ASSERT_EQ(cmq_jwks_refresh_attach(&r, c, "http://10.0.0.1:1/jwks",
                                      "/tmp/cmq_jra.pem", 60), 0);
    ASSERT(r != NULL);
    ASSERT_EQ(cmq_jwks_refresh_interval(r), 60u);
    cmq_jwks_url_t snap;
    memset(&snap, 0, sizeof(snap));
    ASSERT_EQ(cmq_jwks_refresh_snapshot(r, &snap), 0);
    ASSERT_STR_EQ(snap.host, "10.0.0.1");
    ASSERT_STR_EQ(snap.path, "/jwks");
    ASSERT_EQ(snap.port, 1);
    ASSERT_STR_EQ(snap.ca, "/tmp/cmq_jra.pem");
    cmq_jwks_refresher_t *same = r;
    ASSERT_EQ(cmq_jwks_refresh_attach(&r, c, "http://10.0.0.2/jwks",
                                      NULL, 120), 0);
    ASSERT(r == same);
    ASSERT_EQ(cmq_jwks_refresh_interval(r), 60u);
    cmq_jwks_refresh_stop(r);
    cmq_jwks_cache_destroy(c);
}

TEST(jra, omitted) {
    cmq_jwks_cache_t *c = cmq_jwks_cache_create();
    cmq_jwks_refresher_t *r = NULL;
    ASSERT_EQ(cmq_jwks_refresh_attach(&r, c, "http://127.0.0.1/jwks",
                                      NULL, 0), 0);
    ASSERT(r == NULL);
    cmq_jwks_cache_destroy(c);
}

TEST(jra, empty) {
    cmq_jwks_cache_t *c = cmq_jwks_cache_create();
    cmq_jwks_refresher_t *r = NULL;
    ASSERT_EQ(cmq_jwks_refresh_attach(&r, c, "", NULL, 60), 0);
    ASSERT(r == NULL);
    cmq_jwks_cache_destroy(c);
}

TEST(jra, reject) {
    cmq_jwks_cache_t *c = cmq_jwks_cache_create();
    cmq_jwks_refresher_t *r = NULL;
    ASSERT(cmq_jwks_refresh_attach(&r, c, "ftp://127.0.0.1/jwks",
                                   NULL, 60) != 0);
    ASSERT(r == NULL);
    ASSERT(cmq_jwks_refresh_attach(&r, c, "http://127.0.0.1/jwks",
                                   NULL, 4) != 0);
    ASSERT(r == NULL);
    ASSERT(cmq_jwks_refresh_attach(NULL, c, "http://127.0.0.1/jwks",
                                   NULL, 60) != 0);
    cmq_jwks_cache_destroy(c);
}

TEST_MAIN()
