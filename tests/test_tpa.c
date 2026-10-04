/* v0.5.153: reload copies TLS paths onto live config. */
#include "cmq_dynreload.h"
#include "cmq_test.h"
#include <stdlib.h>
#include <string.h>

TEST(tpa, apply) {
    cmq_config_t live;
    memset(&live, 0, sizeof(live));
    live.tls_cert = strdup("/old.pem");
    live.tls_key = strdup("/old.key");
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.tls_cert = "/new.pem";
    fresh.tls_key = "/new.key";
    ASSERT_EQ(cmq_reload_apply_tls_live(&live, &fresh), 0);
    ASSERT_STR_EQ(live.tls_cert, "/new.pem");
    ASSERT_STR_EQ(live.tls_key, "/new.key");
    ASSERT_EQ(cmq_reload_apply_tls_live(&live, &fresh), 0);
    ASSERT_STR_EQ(live.tls_cert, "/new.pem");
    free((void *)live.tls_cert);
    free((void *)live.tls_key);
}

TEST(tpa, omitted) {
    cmq_config_t live;
    memset(&live, 0, sizeof(live));
    live.tls_cert = strdup("/keep.pem");
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    ASSERT_EQ(cmq_reload_apply_tls_live(&live, &fresh), 0);
    ASSERT_STR_EQ(live.tls_cert, "/keep.pem");
    free((void *)live.tls_cert);
}

TEST(tpa, empty) {
    cmq_config_t live;
    memset(&live, 0, sizeof(live));
    live.listeners[1].tls_cert = strdup("/l1.pem");
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.listeners[1].tls_cert = "";
    ASSERT_EQ(cmq_reload_apply_tls_live(&live, &fresh), 0);
    ASSERT_STR_EQ(live.listeners[1].tls_cert, "/l1.pem");
    free((void *)live.listeners[1].tls_cert);
}

TEST(tpa, reject) {
    cmq_config_t live;
    memset(&live, 0, sizeof(live));
    live.tls_cert = strdup("/keep.pem");
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.tls_cert = "../evil.pem";
    ASSERT(cmq_reload_apply_tls_live(&live, &fresh) != 0);
    ASSERT_STR_EQ(live.tls_cert, "/keep.pem");
    ASSERT(cmq_reload_apply_tls_live(NULL, &fresh) != 0);
    fresh.tls_cert = "/ok.pem";
    fresh.tls_key = "bad\\key.pem";
    ASSERT(cmq_reload_apply_tls_live(&live, &fresh) != 0);
    ASSERT_STR_EQ(live.tls_cert, "/keep.pem");
    free((void *)live.tls_cert);
}

TEST_MAIN()
