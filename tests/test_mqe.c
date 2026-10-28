/* v0.5.136: reload applies MQTT bridge addr/port. */
#include "cmq_mqtt.h"
#include "cmq_test.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int listen_loopback(int *port_out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int on = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    socklen_t n = sizeof(sa);
    if (getsockname(fd, (struct sockaddr *)&sa, &n) != 0) {
        close(fd);
        return -1;
    }
    *port_out = ntohs(sa.sin_port);
    return fd;
}

static void *dummy_hold(void *arg) {
    int lfd = *(int *)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) return NULL;
    uint8_t tmp[512];
    (void)recv(cfd, tmp, sizeof(tmp), 0);
    uint8_t ack[] = { 0x20, 0x02, 0x00, 0x00 };
    (void)send(cfd, ack, 4, 0);
    (void)recv(cfd, tmp, sizeof(tmp), 0);
    close(cfd);
    return NULL;
}

struct auth_dummy {
    int lfd;
    uint8_t got[2][256];
    size_t gotn[2];
};

static int read_full(int fd, uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = recv(fd, buf + off, len - off, 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void *auth_dummy_main(void *arg) {
    struct auth_dummy *d = arg;
    for (int i = 0; i < 2; i++) {
        int cfd = accept(d->lfd, NULL, NULL);
        if (cfd < 0) return NULL;
        uint8_t header[2];
        if (read_full(cfd, header, sizeof(header)) == 0 &&
            header[0] == 0x10 && header[1] < sizeof(d->got[i]) - 2) {
            d->got[i][0] = header[0];
            d->got[i][1] = header[1];
            d->gotn[i] = (size_t)header[1] + 2;
            if (read_full(cfd, d->got[i] + 2, header[1]) != 0)
                d->gotn[i] = 0;
        }
        uint8_t ack[] = { 0x20, 0x02, 0x00, 0x00 };
        (void)send(cfd, ack, sizeof(ack), 0);
        close(cfd);
    }
    return NULL;
}

TEST(mqe, apply) {
    int port_a = 0, port_b = 0;
    int lfd_a = listen_loopback(&port_a);
    int lfd_b = listen_loopback(&port_b);
    ASSERT(lfd_a >= 0 && lfd_b >= 0);
    pthread_t tha, thb;
    ASSERT_EQ(pthread_create(&tha, NULL, dummy_hold, &lfd_a), 0);
    ASSERT_EQ(pthread_create(&thb, NULL, dummy_hold, &lfd_b), 0);
    cmq_mqtt_bridge_t *br = cmq_mqtt_bridge_create("mqe");
    ASSERT(br != NULL);
    ASSERT_EQ(cmq_mqtt_bridge_connect(br, "127.0.0.1", port_a), 0);
    char *live = strdup("127.0.0.1");
    int live_port = port_a;
    ASSERT_EQ(cmq_mqtt_reload_endpoint(br, (const char **)&live, &live_port,
                                       NULL, NULL, "127.0.0.1", port_b,
                                       NULL, NULL), 0);
    ASSERT_EQ(live_port, port_b);
    cmq_mqtt_bridge_info_t info = cmq_mqtt_bridge_info(br);
    ASSERT_EQ(info.port, port_b);
    ASSERT_EQ(info.connected, 1);
    cmq_mqtt_bridge_disconnect(br);
    pthread_join(tha, NULL);
    pthread_join(thb, NULL);
    close(lfd_a);
    close(lfd_b);
    free(live);
    cmq_mqtt_bridge_destroy(br);
}

TEST(mqe, reload_auth) {
    int port = 0;
    int lfd = listen_loopback(&port);
    ASSERT(lfd >= 0);
    struct auth_dummy d;
    memset(&d, 0, sizeof(d));
    d.lfd = lfd;
    pthread_t th;
    ASSERT_EQ(pthread_create(&th, NULL, auth_dummy_main, &d), 0);

    cmq_mqtt_bridge_t *br = cmq_mqtt_bridge_create("mqe-auth");
    ASSERT(br != NULL);
    ASSERT_EQ(cmq_mqtt_bridge_connect_auth(br, "127.0.0.1", port,
                                           "old-user", "old-pass"), 0);
    char *live = strdup("127.0.0.1");
    char *live_user = strdup("old-user");
    char *live_pass = strdup("old-pass");
    int live_port = port;
    ASSERT_EQ(cmq_mqtt_reload_endpoint(br, (const char **)&live, &live_port,
                                       (const char **)&live_user,
                                       (const char **)&live_pass,
                                       "127.0.0.1", port,
                                       "new-user", "new-pass"), 0);
    ASSERT_STR_EQ(live_user, "new-user");
    ASSERT_STR_EQ(live_pass, "new-pass");
    pthread_join(th, NULL);
    close(lfd);

    ASSERT_EQ(d.gotn[0], (size_t)42);
    ASSERT_EQ(d.gotn[1], (size_t)42);
    ASSERT_EQ((int)d.got[1][9], 0xC2);
    ASSERT_EQ((int)d.got[1][22], 0x00);
    ASSERT_EQ((int)d.got[1][23], 0x08);
    ASSERT(memcmp(d.got[1] + 24, "new-user", 8) == 0);
    ASSERT_EQ((int)d.got[1][32], 0x00);
    ASSERT_EQ((int)d.got[1][33], 0x08);
    ASSERT(memcmp(d.got[1] + 34, "new-pass", 8) == 0);
    free(live);
    free(live_user);
    free(live_pass);
    cmq_mqtt_bridge_destroy(br);
}

TEST(mqe, omitted) {
    char *live = strdup("10.0.0.1");
    int port = 1883;
    ASSERT_EQ(cmq_mqtt_reload_endpoint(NULL, (const char **)&live, &port,
                                       NULL, NULL, NULL, 0, NULL, NULL), 0);
    ASSERT_STR_EQ(live, "10.0.0.1");
    ASSERT_EQ(port, 1883);
    free(live);
}

TEST(mqe, empty) {
    char *live = strdup("10.0.0.2");
    int port = 1884;
    ASSERT_EQ(cmq_mqtt_reload_endpoint(NULL, (const char **)&live, &port,
                                       NULL, NULL, "", 0, NULL, NULL), 0);
    ASSERT_STR_EQ(live, "10.0.0.2");
    ASSERT_EQ(port, 1884);
    free(live);
}

TEST(mqe, reject) {
    char *live = strdup("10.0.0.3");
    int port = 1885;
    ASSERT(cmq_mqtt_reload_endpoint(NULL, (const char **)&live, &port,
                                     NULL, NULL, "localhost", 1883, NULL, NULL) != 0);
    ASSERT_STR_EQ(live, "10.0.0.3");
    ASSERT_EQ(port, 1885);
    ASSERT(cmq_mqtt_reload_endpoint(NULL, (const char **)&live, &port,
                                     NULL, NULL, "10.0.0.9", 65536, NULL, NULL) != 0);
    ASSERT_EQ(port, 1885);
    ASSERT(cmq_mqtt_reload_endpoint(NULL, (const char **)&live, NULL,
                                     NULL, NULL, "10.0.0.9", 1883, NULL, NULL) != 0);
    free(live);
}

TEST_MAIN()
