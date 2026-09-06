/* v0.5.117: reload applies TLS cert/key paths + cmq_tls_reload. */
#include "cmq_dynreload.h"
#include "cmq_test.h"
#include <string.h>

TEST(tlr, apply) {
    cmq_tls_config_t *slots[4] = {0};
    slots[0] = cmq_tls_config_create();
    ASSERT(slots[0] != NULL);
    ASSERT_EQ(cmq_tls_set_cert(slots[0], "/old.pem"), 0);
    ASSERT_EQ(cmq_tls_set_key(slots[0], "/old.key"), 0);
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.tls_cert = "/new.pem";
    fresh.tls_key = "/new.key";
    ASSERT_EQ(cmq_reload_apply_tls(slots, 4, &fresh), 0);
    char cert[256], key[256];
    ASSERT_EQ(cmq_tls_cert_path(slots[0], cert, sizeof(cert)), 0);
    ASSERT_EQ(cmq_tls_key_path(slots[0], key, sizeof(key)), 0);
    ASSERT_STR_EQ(cert, "/new.pem");
    ASSERT_STR_EQ(key, "/new.key");
    cmq_tls_config_destroy(slots[0]);
}

TEST(tlr, omitted) {
    cmq_tls_config_t *slots[4] = {0};
    slots[0] = cmq_tls_config_create();
    ASSERT_EQ(cmq_tls_set_cert(slots[0], "/keep.pem"), 0);
    ASSERT_EQ(cmq_tls_set_key(slots[0], "/keep.key"), 0);
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    ASSERT_EQ(cmq_reload_apply_tls(slots, 4, &fresh), 0);
    char cert[256];
    ASSERT_EQ(cmq_tls_cert_path(slots[0], cert, sizeof(cert)), 0);
    ASSERT_STR_EQ(cert, "/keep.pem");
    cmq_tls_config_destroy(slots[0]);
}

TEST(tlr, empty) {
    cmq_tls_config_t *slots[4] = {0};
    slots[1] = cmq_tls_config_create();
    ASSERT_EQ(cmq_tls_set_cert(slots[1], "/l1.pem"), 0);
    ASSERT_EQ(cmq_tls_set_key(slots[1], "/l1.key"), 0);
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.listeners[1].tls_cert = "";
    fresh.listeners[1].tls_key = "";
    ASSERT_EQ(cmq_reload_apply_tls(slots, 4, &fresh), 0);
    char cert[256];
    ASSERT_EQ(cmq_tls_cert_path(slots[1], cert, sizeof(cert)), 0);
    ASSERT_STR_EQ(cert, "/l1.pem");
    cmq_tls_config_destroy(slots[1]);
}

TEST(tlr, reject) {
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    ASSERT(cmq_reload_apply_tls(NULL, 4, &fresh) != 0);
    cmq_tls_config_t *slots[4] = {0};
    ASSERT(cmq_reload_apply_tls(slots, 5, &fresh) != 0);
    ASSERT(cmq_reload_apply_tls(slots, -1, &fresh) != 0);
}

TEST_MAIN()
