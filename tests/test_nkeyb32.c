/* v0.5.75: D3 nkey base32 / seed (NATS U… / SU…). */
#include "cmq_test.h"
#include "cmq_jwt.h"
#include <openssl/evp.h>
#include <string.h>
#include <stdio.h>

/* nkeys README: SUAKYRHV… → UD466L6… */
static const char *k_seed =
    "SUAKYRHVIOREXV7EUZTBHUHL7NUMHPMAS7QMDU3GTIUWEI5LDNOXD43IZY";
static const char *k_pub =
    "UD466L6EBCM3YY5HEGHJANNTN4LSKTSUXTH7RILHCKEQMQHTBNLHJJXT";

TEST(nkeyb32, pub_vector) {
    uint8_t pub[32];
    ASSERT_EQ(cmq_nkey_pub_decode(k_pub, pub), 0);
    char enc[64];
    ASSERT_EQ(cmq_nkey_pub_encode(pub, enc, sizeof(enc)), 0);
    ASSERT_STR_EQ(enc, k_pub);
}

TEST(nkeyb32, seed_to_pub) {
    uint8_t seed[32], pub[32], from_u[32];
    ASSERT_EQ(cmq_nkey_seed_decode(k_seed, seed), 0);
    ASSERT_EQ(cmq_nkey_seed_to_pub(seed, pub), 0);
    ASSERT_EQ(cmq_nkey_pub_decode(k_pub, from_u), 0);
    ASSERT(memcmp(pub, from_u, 32) == 0);
}

TEST(nkeyb32, hex_still) {
    uint8_t raw[32];
    memset(raw, 0xab, 32);
    char hex[65];
    static const char *d = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[i * 2] = d[raw[i] >> 4];
        hex[i * 2 + 1] = d[raw[i] & 0xf];
    }
    hex[64] = '\0';
    uint8_t out[32];
    ASSERT_EQ(cmq_nkey_pub_decode(hex, out), 0);
    ASSERT(memcmp(out, raw, 32) == 0);
    ASSERT_EQ(cmq_nkey_verify_user(raw, "u", "00"), -1);
}

TEST(nkeyb32, reject) {
    uint8_t pub[32], seed[32];
    ASSERT(cmq_nkey_pub_decode(NULL, pub) < 0);
    ASSERT(cmq_nkey_pub_decode("not-a-key", pub) < 0);
    char bad[64];
    snprintf(bad, sizeof(bad), "%s", k_pub);
    bad[10] ^= 1;
    ASSERT(cmq_nkey_pub_decode(bad, pub) < 0);
    ASSERT(cmq_nkey_seed_decode("UD466L6EBCM3YY5HEGHJANNTN4LSKTSUXTH7RILHCKEQMQHTBNLHJJXT",
                                seed) < 0);
    ASSERT(cmq_nkey_seed_to_pub(NULL, pub) < 0);
}

TEST_MAIN()
