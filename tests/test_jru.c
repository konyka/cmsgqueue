/* v0.5.137: reload applies jwks_url to the live sidecar. */
#include "cmq_jwksf.h"
#include "cmq_test.h"
#include <stdlib.h>
#include <string.h>

TEST(jru, apply) {
    cmq_jwks_url_t u;
    memset(&u, 0, sizeof(u));
    memcpy(u.host, "127.0.0.1", 10);
    memcpy(u.path, "/jwks", 6);
    u.port = 1;
    cmq_jwks_cache_t *c = cmq_jwks_cache_create();
    ASSERT(c != NULL);
    cmq_jwks_refresher_t *r = cmq_jwks_refresh_start(&u, c, 60);
    ASSERT(r != NULL);
    const char *live = NULL;
    ASSERT_EQ(cmq_jwks_refresh_reload_ca(r, NULL, "/tmp/cmq_jru.pem"), 0);
    ASSERT_EQ(cmq_jwks_refresh_reload_url(r, &live,
                                         "http://10.0.0.1:8443/keys"), 0);
    ASSERT_STR_EQ(live, "http://10.0.0.1:8443/keys");
    cmq_jwks_url_t snap;
    memset(&snap, 0, sizeof(snap));
    ASSERT_EQ(cmq_jwks_refresh_snapshot(r, &snap), 0);
    ASSERT_STR_EQ(snap.host, "10.0.0.1");
    ASSERT_STR_EQ(snap.path, "/keys");
    ASSERT_EQ(snap.port, 8443);
    ASSERT_EQ(snap.tls, 0);
    ASSERT_STR_EQ(snap.ca, "/tmp/cmq_jru.pem");
    free((void *)live);
    cmq_jwks_refresh_stop(r);
    cmq_jwks_cache_destroy(c);
}

TEST(jru, omitted) {
    const char *live = NULL;
    ASSERT_EQ(cmq_jwks_refresh_reload_url(NULL, &live, NULL), 0);
    ASSERT(live == NULL);
}

TEST(jru, empty) {
    char *live = strdup("http://127.0.0.1/jwks");
    ASSERT(live != NULL);
    ASSERT_EQ(cmq_jwks_refresh_reload_url(NULL, (const char **)&live, ""), 0);
    ASSERT_STR_EQ(live, "http://127.0.0.1/jwks");
    free(live);
}

TEST(jru, reject) {
    char *live = strdup("http://127.0.0.1/jwks");
    ASSERT(cmq_jwks_refresh_reload_url(NULL, (const char **)&live,
                                       "ftp://127.0.0.1/jwks") != 0);
    ASSERT_STR_EQ(live, "http://127.0.0.1/jwks");
    ASSERT(cmq_jwks_refresh_reload_url(NULL, (const char **)&live,
                                       "http://127.0.0.1/foo/../x") != 0);
    ASSERT_STR_EQ(live, "http://127.0.0.1/jwks");
    free(live);
}

TEST_MAIN()
