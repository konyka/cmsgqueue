/* v0.5.119: reload applies auth / JWT / nkey live config. */
#include "cmq_dynreload.h"
#include "cmq_test.h"
#include <stdlib.h>
#include <string.h>

static void live_seed(cmq_config_t *live) {
    memset(live, 0, sizeof(*live));
    live->auth_username = strdup("alice");
    live->auth_password = strdup("oldpass");
    live->jwt_issuer = strdup("cmq");
    live->jwt_hmac_secret = strdup("oldsecret");
    live->nkey_pub = strdup("oldnkey");
    live->jwt_ec_pub = strdup("oldec");
    live->jwt_rsa_n = strdup("oldn");
    live->jwt_rsa_e = strdup("olde");
    live->jwt_leeway_sec = 60;
}

static void live_free(cmq_config_t *live) {
    free((void *)live->auth_username);
    free((void *)live->auth_password);
    free((void *)live->jwt_issuer);
    free((void *)live->jwt_hmac_secret);
    free((void *)live->nkey_pub);
    free((void *)live->jwt_ec_pub);
    free((void *)live->jwt_rsa_n);
    free((void *)live->jwt_rsa_e);
}

TEST(ath, apply) {
    cmq_config_t live, fresh;
    live_seed(&live);
    memset(&fresh, 0, sizeof(fresh));
    fresh.auth_username = "bob";
    fresh.auth_password = "newpass";
    fresh.jwt_issuer = "iss2";
    fresh.jwt_hmac_secret = "newsecret";
    fresh.nkey_pub = "newnkey";
    fresh.jwt_ec_pub = "newec";
    fresh.jwt_rsa_n = "newn";
    fresh.jwt_rsa_e = "newe";
    fresh.jwt_leeway_sec = 120;
    ASSERT_EQ(cmq_reload_apply_auth(&live, &fresh), 0);
    ASSERT_STR_EQ(live.auth_username, "bob");
    ASSERT_STR_EQ(live.auth_password, "newpass");
    ASSERT_STR_EQ(live.jwt_issuer, "iss2");
    ASSERT_STR_EQ(live.jwt_hmac_secret, "newsecret");
    ASSERT_STR_EQ(live.nkey_pub, "newnkey");
    ASSERT_STR_EQ(live.jwt_ec_pub, "newec");
    ASSERT_STR_EQ(live.jwt_rsa_n, "newn");
    ASSERT_STR_EQ(live.jwt_rsa_e, "newe");
    ASSERT_EQ(live.jwt_leeway_sec, 120);
    live_free(&live);
}

TEST(ath, omitted) {
    cmq_config_t live, fresh;
    live_seed(&live);
    memset(&fresh, 0, sizeof(fresh));
    ASSERT_EQ(cmq_reload_apply_auth(&live, &fresh), 0);
    ASSERT_STR_EQ(live.auth_username, "alice");
    ASSERT_STR_EQ(live.auth_password, "oldpass");
    ASSERT_STR_EQ(live.jwt_hmac_secret, "oldsecret");
    ASSERT_STR_EQ(live.nkey_pub, "oldnkey");
    ASSERT_EQ(live.jwt_leeway_sec, 60);
    live_free(&live);
}

TEST(ath, empty) {
    cmq_config_t live, fresh;
    live_seed(&live);
    memset(&fresh, 0, sizeof(fresh));
    fresh.auth_username = "";
    fresh.auth_password = "";
    fresh.jwt_hmac_secret = "";
    fresh.nkey_pub = "";
    fresh.jwt_issuer = "";
    ASSERT_EQ(cmq_reload_apply_auth(&live, &fresh), 0);
    ASSERT_STR_EQ(live.auth_username, "alice");
    ASSERT_STR_EQ(live.auth_password, "oldpass");
    ASSERT_STR_EQ(live.jwt_hmac_secret, "oldsecret");
    ASSERT_STR_EQ(live.nkey_pub, "oldnkey");
    live_free(&live);
}

TEST(ath, reject) {
    cmq_config_t live, fresh;
    live_seed(&live);
    memset(&fresh, 0, sizeof(fresh));
    fresh.jwt_hmac_secret = "stolen";
    fresh.jwt_leeway_sec = 3601;
    ASSERT(cmq_reload_apply_auth(&live, &fresh) != 0);
    ASSERT_STR_EQ(live.jwt_hmac_secret, "oldsecret");
    ASSERT_EQ(live.jwt_leeway_sec, 60);
    ASSERT(cmq_reload_apply_auth(NULL, &fresh) != 0);
    ASSERT(cmq_reload_apply_auth(&live, NULL) != 0);
    live_free(&live);
}

TEST_MAIN()
