/* F17: Inter-node TLS.
 *
 * Tests verify the library surface; the server wiring is a
 * follow-up because cmq_route.c currently uses plain read/write
 * syscalls. The cmq_route_tls module provides a TLS wrapper that
 * can be substituted when connecting to a peer configured with
 * route_require_tls.
 */

#include "cmq_test.h"
#include "cmq_route_tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ROUTE_TLS_TEST_DIR "/tmp/cmq-test-route-tls"
#define ROUTE_TLS_CERT     ROUTE_TLS_TEST_DIR "/cert.pem"
#define ROUTE_TLS_KEY      ROUTE_TLS_TEST_DIR "/key.pem"

static void ensure_dir(void) {
    system("rm -rf " ROUTE_TLS_TEST_DIR " && mkdir -p " ROUTE_TLS_TEST_DIR);
}

static void gen_cert(const char *cert, const char *key, const char *cn) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s "
        "-days 1 -nodes -subj '/CN=%s' 2>/dev/null",
        key, cert, cn);
    int rc = system(cmd);
    (void)rc;
}

TEST(route_tls, library_create_destroy) {
    cmq_route_tls_config_t *c = cmq_route_tls_config_create();
    ASSERT_NOT_NULL(c);
    cmq_route_tls_config_destroy(c);
}

TEST(route_tls, set_cert_and_key) {
    ensure_dir();
    gen_cert(ROUTE_TLS_CERT, ROUTE_TLS_KEY, "test");
    cmq_route_tls_config_t *c = cmq_route_tls_config_create();
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(cmq_route_tls_set_cert(c, ROUTE_TLS_CERT), 0);
    ASSERT_EQ(cmq_route_tls_set_key(c, ROUTE_TLS_KEY), 0);
    cmq_route_tls_config_destroy(c);
}

TEST(route_tls, configured_flag) {
    cmq_route_tls_config_t *c = cmq_route_tls_config_create();
    ASSERT_EQ(cmq_route_tls_configured(c), 0);
    cmq_route_tls_config_destroy(c);
}

TEST(route_tls, available_flag) {
    /* F17: returns 1 when OpenSSL is available. */
    ASSERT_EQ(cmq_route_tls_available(), 1);
}

TEST_MAIN()
