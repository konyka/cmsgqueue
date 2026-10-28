/* v0.5.146: reload binds extra listeners when create had none. */
#include "cmq_h2.h"
#include "cmq_test.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

TEST(lbn, apply) {
    int probe = cmq_h2_listen("127.0.0.1", 0);
    ASSERT(probe >= 0);
    int port = cmq_h2_listen_port(probe);
    ASSERT(port > 0);
    close(probe);
    int lfd = -1;
    const char *host = NULL;
    int live = 0;
    ASSERT_EQ(cmq_listener_reload_bind(&lfd, &host, &live,
                                       "127.0.0.1", port, 0), 0);
    ASSERT(lfd >= 0);
    ASSERT_STR_EQ(host, "127.0.0.1");
    ASSERT_EQ(live, port);
    ASSERT_EQ(cmq_h2_listen_port(lfd), port);
    int same = lfd;
    ASSERT_EQ(cmq_listener_reload_bind(&lfd, &host, &live,
                                       "10.0.0.1", port + 1, 0), 0);
    ASSERT(lfd == same);
    ASSERT_EQ(live, port);
    close(lfd);
    free((void *)host);
}

TEST(lbn, omitted) {
    int lfd = -1;
    const char *host = NULL;
    int live = 0;
    ASSERT_EQ(cmq_listener_reload_bind(&lfd, &host, &live, NULL, 0, 0), 0);
    ASSERT_EQ(lfd, -1);
}

TEST(lbn, ipv6) {
    int probe = socket(AF_INET6, SOCK_STREAM, 0);
    ASSERT(probe >= 0);
    struct sockaddr_in6 probe_addr = {0};
    probe_addr.sin6_family = AF_INET6;
    probe_addr.sin6_addr = in6addr_loopback;
    ASSERT_EQ(bind(probe, (struct sockaddr *)&probe_addr, sizeof(probe_addr)), 0);
    ASSERT_EQ(listen(probe, 8), 0);
    socklen_t probe_len = sizeof(probe_addr);
    ASSERT_EQ(getsockname(probe, (struct sockaddr *)&probe_addr, &probe_len), 0);
    int port = ntohs(probe_addr.sin6_port);
    ASSERT(port > 0);
    close(probe);

    int lfd = -1;
    const char *host = NULL;
    int live = 0;
    ASSERT_EQ(cmq_listener_reload_bind(&lfd, &host, &live, "::1", port, 0), 0);
    ASSERT(lfd >= 0);
    ASSERT_STR_EQ(host, "::1");
    ASSERT_EQ(live, port);
    ASSERT_EQ(cmq_h2_listen_port(lfd), port);
    close(lfd);
    free((void *)host);
}

TEST(lbn, empty) {
    int lfd = -1;
    const char *host = NULL;
    int live = 9;
    ASSERT_EQ(cmq_listener_reload_bind(&lfd, &host, &live, "", 0, 0), 0);
    ASSERT_EQ(lfd, -1);
    ASSERT_EQ(live, 9);
}

TEST(lbn, reject) {
    int lfd = -1;
    char *host = strdup("127.0.0.1");
    int live = 9;
    ASSERT(cmq_listener_reload_bind(&lfd, (const char **)&host, &live,
                                    "localhost", 1, 0) != 0);
    ASSERT_EQ(lfd, -1);
    ASSERT_STR_EQ(host, "127.0.0.1");
    ASSERT_EQ(live, 9);
    ASSERT(cmq_listener_reload_bind(&lfd, (const char **)&host, &live,
                                    "127.0.0.1", 65536, 0) != 0);
    ASSERT(cmq_listener_reload_bind(NULL, (const char **)&host, &live,
                                    "127.0.0.1", 1, 0) != 0);
    free(host);
}

TEST_MAIN()
