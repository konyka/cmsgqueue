/* v0.5.170: WS upgrade negotiates permessage-deflate when the client offers it. */
#include "cmq_ws.h"
#include "cmq_test.h"
#include <string.h>

TEST(wsa, apply) {
    const char req[] =
        "GET / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate; "
        "server_no_context_takeover\r\n"
        "\r\n";
    char ext[192];
    ASSERT_EQ(cmq_ws_negotiate_deflate(req, sizeof(req) - 1, ext, sizeof(ext)),
              1);
    ASSERT(strstr(ext, "permessage-deflate") != NULL);
    ASSERT(strstr(ext, "server_no_context_takeover") != NULL);
}

TEST(wsa, omitted) {
    const char req[] =
        "GET / HTTP/1.1\r\n"
        "Upgrade: websocket\r\n"
        "\r\n";
    char ext[192];
    memset(ext, 0x5a, sizeof(ext));
    ASSERT_EQ(cmq_ws_negotiate_deflate(req, sizeof(req) - 1, ext, sizeof(ext)),
              0);
    ASSERT_EQ(ext[0], 0);
}

TEST(wsa, empty) {
    char ext[64];
    memset(ext, 0x5a, sizeof(ext));
    ASSERT_EQ(cmq_ws_negotiate_deflate(NULL, 0, ext, sizeof(ext)), 0);
    ASSERT_EQ(ext[0], 0);
    ASSERT_EQ(cmq_ws_negotiate_deflate("", 0, ext, sizeof(ext)), 0);
}

TEST(wsa, reject) {
    ASSERT(cmq_ws_negotiate_deflate("x", 1, NULL, 8) != 0);
    const char bad[] =
        "Sec-WebSocket-Extensions: permessage-deflate; "
        "server_max_window_bits=12\r\n\r\n";
    char ext[192];
    ASSERT(cmq_ws_negotiate_deflate(bad, sizeof(bad) - 1, ext, sizeof(ext)) != 0);
}

TEST_MAIN()
