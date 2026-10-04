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
                                       "127.0.0.1", port_b), 0);
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

TEST(mqe, omitted) {
    char *live = strdup("10.0.0.1");
    int port = 1883;
    ASSERT_EQ(cmq_mqtt_reload_endpoint(NULL, (const char **)&live, &port,
                                       NULL, 0), 0);
    ASSERT_STR_EQ(live, "10.0.0.1");
    ASSERT_EQ(port, 1883);
    free(live);
}

TEST(mqe, empty) {
    char *live = strdup("10.0.0.2");
    int port = 1884;
    ASSERT_EQ(cmq_mqtt_reload_endpoint(NULL, (const char **)&live, &port,
                                       "", 0), 0);
    ASSERT_STR_EQ(live, "10.0.0.2");
    ASSERT_EQ(port, 1884);
    free(live);
}

TEST(mqe, reject) {
    char *live = strdup("10.0.0.3");
    int port = 1885;
    ASSERT(cmq_mqtt_reload_endpoint(NULL, (const char **)&live, &port,
                                    "localhost", 1883) != 0);
    ASSERT_STR_EQ(live, "10.0.0.3");
    ASSERT_EQ(port, 1885);
    ASSERT(cmq_mqtt_reload_endpoint(NULL, (const char **)&live, &port,
                                    "10.0.0.9", 65536) != 0);
    ASSERT_EQ(port, 1885);
    ASSERT(cmq_mqtt_reload_endpoint(NULL, (const char **)&live, NULL,
                                    "10.0.0.9", 1883) != 0);
    free(live);
}

TEST_MAIN()
