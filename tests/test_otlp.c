/* v0.5.64: D1 OTLP/HTTP JSON encode + URL parse. */
#include "cmq_test.h"
#include "cmq_otlp.h"
#include <string.h>
#include <stdio.h>

TEST(otlp, encode_json) {
    cmq_otel_span_t s;
    memset(&s, 0, sizeof(s));
    memset(s.trace, 0xab, 16);
    s.kind = CMQ_OTEL_KIND_PUBLISH;
    s.t_ms = 42;
    char buf[CMQ_OTLP_JSON_MAX];
    int n = cmq_otlp_encode_json(&s, 1, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(strstr(buf, "resourceSpans") != NULL);
    ASSERT(strstr(buf, "abababababababababababababababab") != NULL);
    ASSERT(strstr(buf, "\"name\":\"publish\"") != NULL);
    ASSERT(strstr(buf, "\"kind\":4") != NULL);
    ASSERT(strstr(buf, "42000000") != NULL);
}

TEST(otlp, parse_url) {
    cmq_otlp_url_t u;
    ASSERT_EQ(cmq_otlp_parse_url("http://127.0.0.1:4318/v1/traces", &u), 0);
    ASSERT(strcmp(u.host, "127.0.0.1") == 0);
    ASSERT_EQ(u.port, 4318);
    ASSERT(strcmp(u.path, "/v1/traces") == 0);
    ASSERT_EQ(cmq_otlp_parse_url("http://10.0.0.8", &u), 0);
    ASSERT_EQ(u.port, CMQ_OTLP_DEFAULT_PORT);
    ASSERT(strcmp(u.path, "/v1/traces") == 0);
}

TEST(otlp, build_request) {
    cmq_otlp_url_t u;
    ASSERT_EQ(cmq_otlp_parse_url("http://127.0.0.1:4318/v1/traces", &u), 0);
    char req[1024];
    ASSERT(cmq_otlp_build_request(&u, "{\"n\":1}", req, sizeof(req)) > 0);
    ASSERT(strstr(req, "POST /v1/traces HTTP/1.1") != NULL);
    ASSERT(strstr(req, "Host: 127.0.0.1:4318") != NULL);
    ASSERT(strstr(req, "Content-Type: application/json") != NULL);
    ASSERT(strstr(req, "Content-Length: 7") != NULL);
    ASSERT(strstr(req, "\r\n\r\n{\"n\":1}") != NULL);
}

TEST(otlp, reject) {
    cmq_otlp_url_t u;
    ASSERT(cmq_otlp_parse_url(NULL, &u) != 0);
    ASSERT(cmq_otlp_parse_url("https://127.0.0.1/v1/traces", &u) != 0);
    ASSERT(cmq_otlp_parse_url("http://user:pass@127.0.0.1/x", &u) != 0);
    ASSERT(cmq_otlp_parse_url("http://127.0.0.1/../etc", &u) != 0);
    char buf[32];
    cmq_otel_span_t s;
    memset(&s, 0, sizeof(s));
    s.kind = CMQ_OTEL_KIND_CONNECT;
    ASSERT(cmq_otlp_encode_json(&s, 1, buf, sizeof(buf)) < 0);
    ASSERT(cmq_otlp_encode_json(NULL, 1, buf, 256) < 0);
}

TEST_MAIN()
