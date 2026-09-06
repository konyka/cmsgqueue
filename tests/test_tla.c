/* v0.5.148: reload attaches TLS when create had none. */
#include "cmq_test.h"
#include "cmq_tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TLA_CERT "build-tdd/tla_cert.pem"
#define TLA_KEY  "build-tdd/tla_key.pem"

static int gen_cert(void) {
    (void)system("mkdir -p build-tdd");
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s "
             "-days 1 -nodes -subj /CN=tla 2>/dev/null",
             TLA_KEY, TLA_CERT);
    return system(cmd) == 0 && access(TLA_CERT, R_OK) == 0 &&
           access(TLA_KEY, R_OK) == 0;
}

TEST(tla, apply) {
    ASSERT(gen_cert());
    cmq_tls_config_t *slot = NULL;
    char *cert = NULL;
    char *key = NULL;
    char *ca = NULL;
    int verify = 0;
    ASSERT_EQ(cmq_tls_reload_attach(&slot, (const char **)&cert,
                                    (const char **)&key, (const char **)&ca,
                                    &verify, TLA_CERT, TLA_KEY, NULL, 0,
                                    NULL), 0);
    ASSERT(slot != NULL);
    ASSERT_STR_EQ(cert, TLA_CERT);
    ASSERT_STR_EQ(key, TLA_KEY);
    ASSERT_EQ(cmq_tls_configured(slot), 1);
    cmq_tls_config_t *same = slot;
    ASSERT_EQ(cmq_tls_reload_attach(&slot, (const char **)&cert,
                                    (const char **)&key, (const char **)&ca,
                                    &verify, "/other.pem", "/other.key",
                                    NULL, 1, NULL), 0);
    ASSERT(slot == same);
    ASSERT_STR_EQ(cert, TLA_CERT);
    cmq_tls_config_destroy(slot);
    free(cert);
    free(key);
}

TEST(tla, omitted) {
    cmq_tls_config_t *slot = NULL;
    char *cert = NULL;
    char *key = NULL;
    char *ca = NULL;
    int verify = 0;
    ASSERT_EQ(cmq_tls_reload_attach(&slot, (const char **)&cert,
                                    (const char **)&key, (const char **)&ca,
                                    &verify, NULL, NULL, NULL, 0, NULL), 0);
    ASSERT(slot == NULL);
}

TEST(tla, empty) {
    cmq_tls_config_t *slot = NULL;
    char *cert = NULL;
    char *key = NULL;
    char *ca = NULL;
    int verify = 0;
    ASSERT_EQ(cmq_tls_reload_attach(&slot, (const char **)&cert,
                                    (const char **)&key, (const char **)&ca,
                                    &verify, "", "", "", 0, NULL), 0);
    ASSERT(slot == NULL);
}

TEST(tla, reject) {
    cmq_tls_config_t *slot = NULL;
    char *cert = strdup("/keep.pem");
    char *key = strdup("/keep.key");
    char *ca = NULL;
    int verify = 0;
    ASSERT(cmq_tls_reload_attach(&slot, (const char **)&cert,
                                 (const char **)&key, (const char **)&ca,
                                 &verify, "../evil.pem", "/k.pem",
                                 NULL, 0, NULL) != 0);
    ASSERT(slot == NULL);
    ASSERT_STR_EQ(cert, "/keep.pem");
    ASSERT(cmq_tls_reload_attach(NULL, (const char **)&cert,
                                 (const char **)&key, (const char **)&ca,
                                 &verify, TLA_CERT, TLA_KEY,
                                 NULL, 0, NULL) != 0);
    ASSERT(cmq_tls_reload_attach(&slot, (const char **)&cert,
                                 (const char **)&key, (const char **)&ca,
                                 &verify, TLA_CERT, NULL,
                                 NULL, 0, NULL) != 0);
    ASSERT(slot == NULL);
    free(cert);
    free(key);
}

TEST_MAIN()
