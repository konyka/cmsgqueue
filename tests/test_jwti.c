/* v0.5.90: JWT HS256 token issuing. */
#include "cmq_test.h"
#include "cmq_jwt.h"
#include <string.h>

TEST(jwti, sign_verify) {
    char tok[CMQ_JWT_TOKEN_MAX];
    ASSERT_EQ(cmq_jwt_sign_hs256("s3cret", "cmq", "alice", 2000000000ull,
                                 tok, sizeof(tok)), 0);
    ASSERT(tok[0] != '\0');
    char sub[32] = {0};
    ASSERT_EQ(cmq_jwt_verify_hs256(tok, "s3cret", "cmq", 1700000000ull, 60,
                                   sub, sizeof(sub)), 0);
    ASSERT_STR_EQ(sub, "alice");
}

TEST(jwti, bad_secret) {
    char tok[CMQ_JWT_TOKEN_MAX];
    ASSERT_EQ(cmq_jwt_sign_hs256("s3cret", "cmq", "bob", 2000000000ull,
                                 tok, sizeof(tok)), 0);
    ASSERT(cmq_jwt_verify_hs256(tok, "wrong", "cmq", 1700000000ull, 60,
                                NULL, 0) != 0);
}

TEST(jwti, expired) {
    char tok[CMQ_JWT_TOKEN_MAX];
    ASSERT_EQ(cmq_jwt_sign_hs256("s3cret", "cmq", "carol", 100ull,
                                 tok, sizeof(tok)), 0);
    ASSERT_EQ(cmq_jwt_verify_hs256(tok, "s3cret", "cmq", 100000ull, 60,
                                   NULL, 0), -3);
}

TEST(jwti, reject) {
    char tok[64];
    ASSERT(cmq_jwt_sign_hs256(NULL, "cmq", "a", 1, tok, sizeof(tok)) != 0);
    ASSERT(cmq_jwt_sign_hs256("s", NULL, "a", 1, tok, sizeof(tok)) != 0);
    ASSERT(cmq_jwt_sign_hs256("s", "cmq", NULL, 1, tok, sizeof(tok)) != 0);
    ASSERT(cmq_jwt_sign_hs256("s", "cmq", "a", 0, tok, sizeof(tok)) != 0);
    ASSERT(cmq_jwt_sign_hs256("s", "cm\"q", "a", 1, tok, sizeof(tok)) != 0);
    ASSERT(cmq_jwt_sign_hs256("s", "cmq", "a", 1, NULL, 8) != 0);
    ASSERT(cmq_jwt_sign_hs256("s", "cmq", "a", 1, tok, 8) != 0);
}

TEST_MAIN()
