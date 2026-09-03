/* v0.5.22: WS permessage-deflate roundtrip + extension negotiation. */

#include "cmq_test.h"
#include "cmq_ws.h"

#include <stdio.h>
#include <string.h>

TEST(ws_deflate, extensions_detect) {
    const char req[] =
        "GET /chat HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover\r\n"
        "\r\n";
    ASSERT_EQ(cmq_ws_parse_extensions(req, sizeof(req) - 1), 1);
}

TEST(ws_deflate, extensions_reject_unknown_param) {
    /* Unknown parameter must cause rejection (return -1). */
    const char req[] =
        "GET /chat HTTP/1.1\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate; server_max_window_bits=99\r\n"
        "\r\n";
    ASSERT_EQ(cmq_ws_parse_extensions(req, sizeof(req) - 1), -1);
}

TEST(ws_deflate, extensions_absent) {
    const char req[] =
        "GET /chat HTTP/1.1\r\n"
        "Upgrade: websocket\r\n"
        "\r\n";
    ASSERT_EQ(cmq_ws_parse_extensions(req, sizeof(req) - 1), 0);
}

TEST(ws_deflate, roundtrip_compressible) {
    /* Repetitive JSON payload — typical WS pub/sub notification. */
    const uint8_t in[] =
        "{\"subject\":\"orders.new\",\"data\":{\"id\":12345,\"total\":99.99}}"
        "{\"subject\":\"orders.new\",\"data\":{\"id\":12346,\"total\":100.00}}"
        "{\"subject\":\"orders.new\",\"data\":{\"id\":12347,\"total\":101.50}}";
    uint8_t comp[2048];
    int comp_len = cmq_ws_deflate_message(in, sizeof(in) - 1, comp, sizeof(comp));
    ASSERT_TRUE(comp_len > 0);

    ASSERT_TRUE((size_t)comp_len < sizeof(in) - 1);

    uint8_t out[2048];
    int out_len = cmq_ws_inflate_message(comp, (size_t)comp_len, out, sizeof(out));
    ASSERT_EQ(out_len, (int)(sizeof(in) - 1));
    ASSERT_MEM_EQ(out, in, sizeof(in) - 1);
}

TEST(ws_deflate, roundtrip_random_byte_aligned) {
    /* Pseudo-random data still round-trips, may even grow due to overhead. */
    uint8_t in[256];
    for (size_t i = 0; i < sizeof(in); i++)
        in[i] = (uint8_t)(i * 37 + 11);

    uint8_t comp[2048];
    int comp_len = cmq_ws_deflate_message(in, sizeof(in), comp, sizeof(comp));
    ASSERT_TRUE(comp_len > 0);

    uint8_t out[2048];
    int out_len = cmq_ws_inflate_message(comp, (size_t)comp_len, out, sizeof(out));
    ASSERT_EQ(out_len, (int)sizeof(in));
    ASSERT_MEM_EQ(out, in, sizeof(in));
}

TEST(ws_deflate, inflate_rejects_garbage) {
    /* Corrupt compressed stream → Z_DATA_ERROR → -1. */
    uint8_t bad[16] = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8,
                       0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF};
    uint8_t out[256];
    ASSERT_EQ(cmq_ws_inflate_message(bad, sizeof(bad), out, sizeof(out)), -1);
}

TEST(ws_deflate, two_messages_independent) {
    /* Two consecutive compressed messages each inflate on their own — proves
     * per-message flush boundary is preserved. */
    const uint8_t msg1[] = "hello world hello world hello world";
    const uint8_t msg2[] = "{\"k\":\"v\",\"x\":42}";

    uint8_t c1[256], c2[256];
    int c1_len = cmq_ws_deflate_message(msg1, sizeof(msg1) - 1, c1, sizeof(c1));
    int c2_len = cmq_ws_deflate_message(msg2, sizeof(msg2) - 1, c2, sizeof(c2));
    ASSERT_TRUE(c1_len > 0);
    ASSERT_TRUE(c2_len > 0);

    uint8_t out1[256], out2[256];
    int o1 = cmq_ws_inflate_message(c1, (size_t)c1_len, out1, sizeof(out1));
    int o2 = cmq_ws_inflate_message(c2, (size_t)c2_len, out2, sizeof(out2));
    ASSERT_EQ(o1, (int)(sizeof(msg1) - 1));
    ASSERT_EQ(o2, (int)(sizeof(msg2) - 1));
    ASSERT_MEM_EQ(out1, msg1, sizeof(msg1) - 1);
    ASSERT_MEM_EQ(out2, msg2, sizeof(msg2) - 1);
}

TEST(ws_deflate, build_response_includes_extension) {
    char out[256];
    int n = cmq_ws_build_extensions_response(out, sizeof(out));
    ASSERT_TRUE(n > 0);
    /* Must contain the permessage-deflate token and a context-takeover param. */
    ASSERT(strstr(out, "permessage-deflate") != NULL);
    ASSERT(strstr(out, "server_no_context_takeover") != NULL ||
           strstr(out, "client_no_context_takeover") != NULL);
}

TEST_MAIN()
