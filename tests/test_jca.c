/* v0.5.134: reload applies jwks_ca to the live sidecar. */
#include "cmq_jwksf.h"
#include "cmq_test.h"
#include <stdlib.h>
#include <string.h>

TEST(jca, apply) {
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
    ASSERT_EQ(cmq_jwks_refresh_reload_ca(r, &live, "/tmp/cmq_jca.pem"), 0);
    ASSERT_STR_EQ(live, "/tmp/cmq_jca.pem");
    char path[256];
    ASSERT_EQ(cmq_jwks_refresh_ca(r, path, sizeof(path)), 0);
    ASSERT_STR_EQ(path, "/tmp/cmq_jca.pem");
    free((void *)live);
    cmq_jwks_refresh_stop(r);
    cmq_jwks_cache_destroy(c);
}

TEST(jca, omitted) {
    const char *live = NULL;
    ASSERT_EQ(cmq_jwks_refresh_reload_ca(NULL, &live, NULL), 0);
    ASSERT(live == NULL);
}

TEST(jca, empty) {
    char *live = strdup("/old.pem");
    ASSERT(live != NULL);
    ASSERT_EQ(cmq_jwks_refresh_reload_ca(NULL, (const char **)&live, ""), 0);
    ASSERT_STR_EQ(live, "/old.pem");
    free(live);
}

TEST(jca, reject) {
    char *live = strdup("/keep.pem");
    ASSERT(cmq_jwks_refresh_reload_ca(NULL, (const char **)&live,
                                      "../evil.pem") != 0);
    ASSERT_STR_EQ(live, "/keep.pem");
    free(live);
}

TEST_MAIN()
