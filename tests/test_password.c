/* F8: scrypt password hashing.
 *
 * Wire format: $scrypt$N=2^14,r=8,p=1$<salt-b64>$<hash-b64>
 * Scrypt parameters chosen to be ~100ms on a modern x86_64 server.
 * OpenSSL's EVP_PBE_scrypt is used for both hash and verify.
 *
 * Legacy plaintext passwords: stored under $plaintext$ prefix during
 * a transition window; the verify path detects this and accepts.
 * Production deployments should use cmq_config_set_password_hash()
 * to migrate.
 */

#include "cmq_test.h"
#include "cmq_password.h"
#include <string.h>
#include <stdlib.h>

TEST(password, hash_format_round_trip) {
    char hash[256];
    int rc = cmq_password_hash("correct horse battery staple", hash, sizeof(hash));
    ASSERT_EQ(rc, 0);
    /* Verify the wire format prefix. */
    ASSERT(strncmp(hash, "$scrypt$", 8) == 0);
    /* Verify the prefix fields. */
    ASSERT(strstr(hash, "N=16384") != NULL);
    ASSERT(strstr(hash, "r=8") != NULL);
    ASSERT(strstr(hash, "p=1") != NULL);
    /* Verify verify() against the same password succeeds. */
    ASSERT_EQ(cmq_password_verify(hash, "correct horse battery staple"), 1);
    /* Verify verify() against a wrong password fails. */
    ASSERT_EQ(cmq_password_verify(hash, "wrong password"), 0);
}

TEST(password, empty_password_rejected) {
    char hash[256];
    int rc = cmq_password_hash("", hash, sizeof(hash));
    /* Empty password is a configuration error. */
    ASSERT(rc != 0);
}

TEST(password, plaintext_legacy_accepted) {
    /* The legacy plaintext path: "$plaintext$xxx" works. */
    const char *legacy = "$plaintext$helloworld";
    ASSERT_EQ(cmq_password_verify(legacy, "helloworld"), 1);
    ASSERT_EQ(cmq_password_verify(legacy, "worldhello"), 0);
}

TEST(password, malformed_hash_rejected) {
    ASSERT_EQ(cmq_password_verify("not-a-valid-hash", "anything"), -1);
    ASSERT_EQ(cmq_password_verify("$scrypt$badformat", "x"), -1);
    ASSERT_EQ(cmq_password_verify("", "x"), -1);
}

TEST(password, buffer_too_small) {
    char small[16];
    int rc = cmq_password_hash("test", small, sizeof(small));
    /* Output does not fit; return error. */
    ASSERT(rc != 0);
}

TEST(password, distinct_passwords_distinct_hashes) {
    char h1[256], h2[256];
    ASSERT_EQ(cmq_password_hash("password1", h1, sizeof(h1)), 0);
    ASSERT_EQ(cmq_password_hash("password2", h2, sizeof(h2)), 0);
    /* Salts are random; hashes differ even if passwords were equal. */
    char h3[256];
    ASSERT_EQ(cmq_password_hash("password1", h3, sizeof(h3)), 0);
    ASSERT(strcmp(h1, h3) != 0);
    ASSERT(strcmp(h1, h2) != 0);
}

TEST_MAIN()
