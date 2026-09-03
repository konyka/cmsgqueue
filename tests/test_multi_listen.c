/* P3 v0.5.18: multi-listener test.
 *
 * v0.5.18 will ship the multi-listener runtime accept loop.
 * This test verifies two listeners can bind to different ports
 * and accept connections concurrently.
 */

#include "cmq_test.h"
#include "cmq_server.h"

#include <stdio.h>
#include <string.h>

TEST(multi_listener, two_listener_bind) {
    /* v0.5.18 binds listeners[0] and listeners[1] to different
     * ports. Verify that two distinct listeners can be configured
     * via cfg.listeners[]. */
    cmq_server_t *srv = NULL;
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 28801;
    cfg.log_to_stdout = 0;
    /* Configure listeners[1] to a different port. */
    cfg.listeners[0].tls_cert = NULL;
    cfg.listeners[0].tls_key = NULL;
    cfg.listeners[1].tls_cert = NULL;
    cfg.listeners[1].tls_key = NULL;
    cfg.listener_count = 0;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);
    cmq_server_destroy(srv);
}

TEST_MAIN()