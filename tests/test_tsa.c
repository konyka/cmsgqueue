/* v0.5.169: reload creates the TLS session cache when load left it NULL. */
#include "cmq_tls.h"
#include "cmq_tls_session_cache.h"
#include "cmq_test.h"

TEST(tsa, apply) {
    cmq_tls_config_t *cfg = cmq_tls_config_create();
    ASSERT_NOT_NULL(cfg);
    ASSERT(cmq_tls_get_session_cache_state(cfg) == NULL);
    ASSERT_EQ(cmq_tls_session_cache_reload_attach(cfg), 0);
    ASSERT_NOT_NULL(cmq_tls_get_session_cache_state(cfg));
    ASSERT_EQ(cmq_tls_session_cache_size(cfg), 0);
    cmq_tls_config_destroy(cfg);
}

TEST(tsa, omitted) {
    cmq_tls_config_t *cfg = cmq_tls_config_create();
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cmq_tls_session_cache_reload_attach(cfg), 0);
    ASSERT_NOT_NULL(cmq_tls_get_session_cache_state(cfg));
    cmq_tls_config_destroy(cfg);
}

TEST(tsa, empty) {
    cmq_tls_config_t *cfg = cmq_tls_config_create();
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cmq_tls_session_cache_reload_attach(cfg), 0);
    void *same = cmq_tls_get_session_cache_state(cfg);
    ASSERT_NOT_NULL(same);
    ASSERT_EQ(cmq_tls_session_cache_reload_attach(cfg), 0);
    ASSERT(cmq_tls_get_session_cache_state(cfg) == same);
    cmq_tls_config_destroy(cfg);
}

TEST(tsa, reject) {
    ASSERT(cmq_tls_session_cache_reload_attach(NULL) != 0);
}

TEST_MAIN()
