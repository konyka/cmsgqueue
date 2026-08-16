/* F12 follow-on: mTLS API surface. */

#include "cmq_test.h"
#include "cmq_tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MTLS_TEST_DIR "/tmp/cmq-test-mtls"
#define MTLS_CA_FILE  MTLS_TEST_DIR "/ca.pem"

static void gen_ca(const char *ca_path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s "
        "-days 1 -nodes -subj '/CN=test-ca' 2>/dev/null",
        ca_path, ca_path);
    int rc = system(cmd);
    (void)rc;
}

TEST(mtls_api, set_ca_and_verify_peer) {
    cmq_tls_config_t *c = cmq_tls_config_create();
    ASSERT_NOT_NULL(c);
    cmq_tls_set_verify(c, 1);
    ASSERT_EQ(cmq_tls_verify_peer(c), 1);
    /* Setting CA path is independent of verify_peer. */
    cmq_tls_set_ca(c, "/tmp/anywhere.pem");
    ASSERT_EQ(cmq_tls_verify_peer(c), 1);
    /* Setting verify off resets the flag. */
    cmq_tls_set_verify(c, 0);
    ASSERT_EQ(cmq_tls_verify_peer(c), 0);
    cmq_tls_config_destroy(c);
}

TEST(mtls_api, ca_path_set_with_real_file) {
    system("rm -rf " MTLS_TEST_DIR " && mkdir -p " MTLS_TEST_DIR);
    gen_ca(MTLS_CA_FILE);
    cmq_tls_config_t *c = cmq_tls_config_create();
    ASSERT_NOT_NULL(c);
    cmq_tls_set_ca(c, MTLS_CA_FILE);
    cmq_tls_set_verify(c, 1);
    ASSERT_EQ(cmq_tls_verify_peer(c), 1);
    cmq_tls_config_destroy(c);
    system("rm -rf " MTLS_TEST_DIR);
}

TEST_MAIN()
