/* v0.5.117: reload applies TLS cert/key paths + cmq_tls_reload. */
#define _POSIX_C_SOURCE 200809L
#include "cmq_dynreload.h"
#include "cmq_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct tlr_fixture {
    char dir[256];
    char old_cert[512];
    char old_key[512];
    char new_cert[512];
    char new_key[512];
};

static int tlr_make_pair(const char *cert, const char *key) {
    char command[1200];
    int written = snprintf(command, sizeof(command),
                           "openssl req -x509 -newkey rsa:2048 -nodes "
                           "-days 1 -subj /CN=cmq-tlr -keyout '%s' "
                           "-out '%s' >/dev/null 2>&1", key, cert);
    if (written < 0 || (size_t)written >= sizeof(command)) return -1;
    return system(command) == 0 ? 0 : -1;
}

static int tlr_fixture_init(struct tlr_fixture *fixture) {
    char template[] = "/tmp/cmq_tlr_XXXXXX";
    char *dir = mkdtemp(template);
    if (!dir) return -1;
    if (snprintf(fixture->dir, sizeof(fixture->dir), "%s", dir) < 0 ||
        snprintf(fixture->old_cert, sizeof(fixture->old_cert),
                 "%s/old-cert.pem", dir) < 0 ||
        snprintf(fixture->old_key, sizeof(fixture->old_key),
                 "%s/old-key.pem", dir) < 0 ||
        snprintf(fixture->new_cert, sizeof(fixture->new_cert),
                 "%s/new-cert.pem", dir) < 0 ||
        snprintf(fixture->new_key, sizeof(fixture->new_key),
                 "%s/new-key.pem", dir) < 0 ||
        tlr_make_pair(fixture->old_cert, fixture->old_key) != 0 ||
        tlr_make_pair(fixture->new_cert, fixture->new_key) != 0) {
        unlink(fixture->old_cert);
        unlink(fixture->old_key);
        unlink(fixture->new_cert);
        unlink(fixture->new_key);
        rmdir(fixture->dir);
        return -1;
    }
    return 0;
}

static void tlr_fixture_cleanup(const struct tlr_fixture *fixture) {
    unlink(fixture->old_cert);
    unlink(fixture->old_key);
    unlink(fixture->new_cert);
    unlink(fixture->new_key);
    rmdir(fixture->dir);
}

TEST(tlr, apply) {
    struct tlr_fixture fixture;
    ASSERT_EQ(tlr_fixture_init(&fixture), 0);
    cmq_tls_config_t *slots[4] = {0};
    slots[0] = cmq_tls_config_create();
    ASSERT(slots[0] != NULL);
    ASSERT_EQ(cmq_tls_set_cert(slots[0], fixture.old_cert), 0);
    ASSERT_EQ(cmq_tls_set_key(slots[0], fixture.old_key), 0);
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.tls_cert = fixture.new_cert;
    fresh.tls_key = fixture.new_key;
    ASSERT_EQ(cmq_reload_apply_tls(slots, 4, &fresh), 0);
    char cert[256], key[256];
    ASSERT_EQ(cmq_tls_cert_path(slots[0], cert, sizeof(cert)), 0);
    ASSERT_EQ(cmq_tls_key_path(slots[0], key, sizeof(key)), 0);
    ASSERT_STR_EQ(cert, fixture.new_cert);
    ASSERT_STR_EQ(key, fixture.new_key);
    cmq_tls_config_destroy(slots[0]);
    tlr_fixture_cleanup(&fixture);
}

TEST(tlr, omitted) {
    struct tlr_fixture fixture;
    ASSERT_EQ(tlr_fixture_init(&fixture), 0);
    cmq_tls_config_t *slots[4] = {0};
    slots[0] = cmq_tls_config_create();
    ASSERT_EQ(cmq_tls_set_cert(slots[0], fixture.old_cert), 0);
    ASSERT_EQ(cmq_tls_set_key(slots[0], fixture.old_key), 0);
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    ASSERT_EQ(cmq_reload_apply_tls(slots, 4, &fresh), 0);
    char cert[256];
    ASSERT_EQ(cmq_tls_cert_path(slots[0], cert, sizeof(cert)), 0);
    ASSERT_STR_EQ(cert, fixture.old_cert);
    cmq_tls_config_destroy(slots[0]);
    tlr_fixture_cleanup(&fixture);
}

TEST(tlr, empty) {
    struct tlr_fixture fixture;
    ASSERT_EQ(tlr_fixture_init(&fixture), 0);
    cmq_tls_config_t *slots[4] = {0};
    slots[1] = cmq_tls_config_create();
    ASSERT_EQ(cmq_tls_set_cert(slots[1], fixture.old_cert), 0);
    ASSERT_EQ(cmq_tls_set_key(slots[1], fixture.old_key), 0);
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.listeners[1].tls_cert = "";
    fresh.listeners[1].tls_key = "";
    ASSERT_EQ(cmq_reload_apply_tls(slots, 4, &fresh), 0);
    char cert[256];
    ASSERT_EQ(cmq_tls_cert_path(slots[1], cert, sizeof(cert)), 0);
    ASSERT_STR_EQ(cert, fixture.old_cert);
    cmq_tls_config_destroy(slots[1]);
    tlr_fixture_cleanup(&fixture);
}

TEST(tlr, reject) {
    struct tlr_fixture fixture;
    ASSERT_EQ(tlr_fixture_init(&fixture), 0);
    cmq_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    ASSERT(cmq_reload_apply_tls(NULL, 4, &fresh) != 0);
    cmq_tls_config_t *slots[4] = {0};
    ASSERT(cmq_reload_apply_tls(slots, 5, &fresh) != 0);
    ASSERT(cmq_reload_apply_tls(slots, -1, &fresh) != 0);
    slots[0] = cmq_tls_config_create();
    ASSERT(slots[0] != NULL);
    ASSERT_EQ(cmq_tls_set_cert(slots[0], fixture.old_cert), 0);
    ASSERT_EQ(cmq_tls_set_key(slots[0], fixture.old_key), 0);
    fresh.tls_cert = "../evil.pem";
    ASSERT(cmq_reload_apply_tls(slots, 4, &fresh) != 0);
    char cert[256];
    ASSERT_EQ(cmq_tls_cert_path(slots[0], cert, sizeof(cert)), 0);
    ASSERT_STR_EQ(cert, fixture.old_cert);
    char missing[512];
    ASSERT(snprintf(missing, sizeof(missing), "%s/missing.pem",
                    fixture.dir) > 0);
    fresh.tls_cert = missing;
    ASSERT(cmq_reload_apply_tls(slots, 4, &fresh) != 0);
    cmq_tls_config_destroy(slots[0]);
    tlr_fixture_cleanup(&fixture);
}

TEST_MAIN()
