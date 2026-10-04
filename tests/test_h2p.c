/* v0.5.81: D2 ALPN h2 + h2_port. */
#include "cmq_test.h"
#include "cmq_h2.h"
#include "cmq_tls.h"

#include <unistd.h>

TEST(h2p, alpn_h2) {
    cmq_tls_config_t *cfg = cmq_tls_config_create();
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cmq_tls_set_alpn(cfg, "h2"), 0);
    ASSERT_EQ(cmq_tls_alpn_has(cfg, "h2"), 1);
    ASSERT_EQ(cmq_tls_alpn_has(cfg, "http/1.1"), 0);
    cmq_tls_config_destroy(cfg);
}

TEST(h2p, listen_port) {
    int lfd = cmq_h2_listen("127.0.0.1", 0);
    ASSERT(lfd >= 0);
    int port = cmq_h2_listen_port(lfd);
    ASSERT(port > 0);
    close(lfd);
}

TEST(h2p, alpn_clear) {
    cmq_tls_config_t *cfg = cmq_tls_config_create();
    ASSERT_EQ(cmq_tls_set_alpn(cfg, "h2"), 0);
    ASSERT_EQ(cmq_tls_set_alpn(cfg, NULL), 0);
    ASSERT_EQ(cmq_tls_alpn_has(cfg, "h2"), 0);
    cmq_tls_config_destroy(cfg);
}

TEST(h2p, reject) {
    ASSERT_EQ(cmq_tls_alpn_has(NULL, "h2"), 0);
    ASSERT(cmq_tls_set_alpn(NULL, "h2") != 0);
    ASSERT(cmq_h2_listen("8.8.8.8", 80) < 0);
    ASSERT(cmq_h2_listen_port(-1) < 0);
}

TEST_MAIN()
