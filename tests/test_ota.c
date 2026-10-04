/* v0.5.139: reload attaches otlp_endpoint when create had none. */
#include "cmq_otlp.h"
#include "cmq_test.h"
#include <stdlib.h>
#include <string.h>

TEST(ota, apply) {
    cmq_otlp_url_t *u = NULL;
    ASSERT_EQ(cmq_otlp_reload_attach(&u, "http://10.0.0.1:4319/v1/traces"), 0);
    ASSERT(u != NULL);
    ASSERT_STR_EQ(u->host, "10.0.0.1");
    ASSERT_STR_EQ(u->path, "/v1/traces");
    ASSERT_EQ(u->port, 4319);
    ASSERT_EQ(u->tls, 0);
    ASSERT_EQ(u->grpc, 0);
    cmq_otlp_url_t *same = u;
    ASSERT_EQ(cmq_otlp_reload_attach(&u, "http://10.0.0.2/v1/traces"), 0);
    ASSERT(u == same);
    ASSERT_STR_EQ(u->host, "10.0.0.1");
    free(u);
}

TEST(ota, omitted) {
    cmq_otlp_url_t *u = NULL;
    ASSERT_EQ(cmq_otlp_reload_attach(&u, NULL), 0);
    ASSERT(u == NULL);
}

TEST(ota, empty) {
    cmq_otlp_url_t *u = NULL;
    ASSERT_EQ(cmq_otlp_reload_attach(&u, ""), 0);
    ASSERT(u == NULL);
}

TEST(ota, reject) {
    cmq_otlp_url_t *u = NULL;
    ASSERT(cmq_otlp_reload_attach(&u, "ftp://127.0.0.1/v1/traces") != 0);
    ASSERT(u == NULL);
    ASSERT(cmq_otlp_reload_attach(&u, "http://127.0.0.1/foo/../x") != 0);
    ASSERT(u == NULL);
    ASSERT(cmq_otlp_reload_attach(NULL, "http://127.0.0.1/v1/traces") != 0);
}

TEST_MAIN()
