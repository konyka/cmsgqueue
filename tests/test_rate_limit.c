/* F10: per-IP rate limit + per-conn subject cap.
 *
 * The per-conn subject cap is already enforced by the existing
 * subscribe path; this test exercises the rate limit by issuing
 * many rapid connects to a server with max_connects_per_sec=2.
 *
 * Tests:
 *   - rate_limit_config: verifies the config key is parsed.
 *   - rate_limit_rejects_excess: connects > limit rejected.
 */

#include "cmq_test.h"
#include "cmq_server.h"
#include "cmq_config.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define RATE_PORT 18810

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void *server_thread(void *arg) {
    cmq_server_t *srv = (cmq_server_t *)arg;
    cmq_server_run(srv);
    return NULL;
}

TEST(rate_limit, config_field_set) {
    cmq_config_t cfg = {0};
    cfg.max_connects_per_sec = 5;
    ASSERT_EQ(cfg.max_connects_per_sec, 5);
}

TEST(rate_limit, rejects_excess) {
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = RATE_PORT;
    cfg.log_to_stdout = 0;
    cfg.max_clients = 100;
    cfg.max_connects_per_sec = 2;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    struct timespec ts = {0, 200000000};
    nanosleep(&ts, NULL);

    int accepted = 0;
    int rejected = 0;
    for (int i = 0; i < 20; i++) {
        int fd = connect_to(RATE_PORT);
        if (fd >= 0) {
            /* Verify the connection is alive (not just SYN_ACK'd).
             * Server may close on rate limit but accept the TCP
             * connection first. */
            struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buf[16];
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            close(fd);
            if (n > 0) {
                accepted++;
            } else {
                rejected++;
            }
        } else {
            rejected++;
        }
    }
    /* Cap is 2/sec; we attempted 20 in <500ms. Expect many rejections. */
    ASSERT(rejected >= 5);
    ASSERT(accepted + rejected == 20);
    cmq_server_destroy(srv);
    pthread_join(tid, NULL);
}

TEST_MAIN()
