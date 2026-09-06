/* v0.5.138: reload applies otlp_endpoint to the live exporter URL. */
#include "cmq_otlp.h"
#include "cmq_test.h"
#include <stdlib.h>
#include <string.h>

TEST(oeu, apply) {
    cmq_otlp_url_t u;
    memset(&u, 0, sizeof(u));
    ASSERT_EQ(cmq_otlp_set_ca(&u, "/tmp/cmq_oeu.pem"), 0);
    const char *live = NULL;
    ASSERT_EQ(cmq_otlp_reload_url(&u, &live,
                                  "http://10.0.0.1:4319/v1/traces"), 0);
    ASSERT_STR_EQ(live, "http://10.0.0.1:4319/v1/traces");
    ASSERT_STR_EQ(u.host, "10.0.0.1");
    ASSERT_STR_EQ(u.path, "/v1/traces");
    ASSERT_EQ(u.port, 4319);
    ASSERT_EQ(u.tls, 0);
    ASSERT_EQ(u.grpc, 0);
    ASSERT_STR_EQ(u.ca, "/tmp/cmq_oeu.pem");
    free((void *)live);
}

TEST(oeu, omitted) {
    const char *live = NULL;
    ASSERT_EQ(cmq_otlp_reload_url(NULL, &live, NULL), 0);
    ASSERT(live == NULL);
}

TEST(oeu, empty) {
    cmq_otlp_url_t u;
    memset(&u, 0, sizeof(u));
    ASSERT_EQ(cmq_otlp_parse_url("http://127.0.0.1/v1/traces", &u), 0);
    char *live = strdup("http://127.0.0.1/v1/traces");
    ASSERT_EQ(cmq_otlp_reload_url(&u, (const char **)&live, ""), 0);
    ASSERT_STR_EQ(live, "http://127.0.0.1/v1/traces");
    ASSERT_STR_EQ(u.host, "127.0.0.1");
    free(live);
}

TEST(oeu, reject) {
    cmq_otlp_url_t u;
    memset(&u, 0, sizeof(u));
    ASSERT_EQ(cmq_otlp_parse_url("http://127.0.0.1/v1/traces", &u), 0);
    char *live = strdup("http://127.0.0.1/v1/traces");
    ASSERT(cmq_otlp_reload_url(&u, (const char **)&live,
                               "ftp://127.0.0.1/v1/traces") != 0);
    ASSERT_STR_EQ(live, "http://127.0.0.1/v1/traces");
    ASSERT_STR_EQ(u.host, "127.0.0.1");
    ASSERT(cmq_otlp_reload_url(&u, (const char **)&live,
                               "http://127.0.0.1/foo/../x") != 0);
    ASSERT_STR_EQ(live, "http://127.0.0.1/v1/traces");
    free(live);
}

TEST_MAIN()
