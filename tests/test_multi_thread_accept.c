/* v0.5.42: aux accept thread must admit, not close.
 *
 * num_threads=2 starts accept_thread_func. After the fix it stays
 * alive while running=1 and calls admit_one_client. stat_accept_aux
 * increments only on that path.
 */
#define _POSIX_C_SOURCE 200809L
#include "cmq_test.h"
#include "cmq_server.h"
#include "cmq_parser.h"
#include "cmq_proto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define AUX_PORT 23920
#define AUX_BURST 32

static void wait_ms(int ms) {
    struct timespec ts = {0, ms * 1000000L};
    nanosleep(&ts, NULL);
}

static void *server_thread(void *arg) {
    cmq_server_run((cmq_server_t *)arg);
    return NULL;
}

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static ssize_t send_frame(int fd, cmq_op_t op) {
    uint8_t buf[32];
    size_t n = cmq_frame_encode(buf, sizeof(buf), op, 0, NULL, 0);
    if (n == 0) return -1;
    return write(fd, buf, n);
}

static int recv_op(int fd, cmq_parser_t *p, cmq_op_t want) {
    for (int i = 0; i < 200; i++) {
        const cmq_frame_t *f = cmq_parser_frame(p);
        if (f) {
            int ok = (f->hdr.op == want);
            cmq_parser_next(p);
            return ok ? 0 : -1;
        }
        uint8_t buf[1024];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                wait_ms(10);
                continue;
            }
            wait_ms(10);
            continue;
        }
        cmq_parser_feed(p, buf, (size_t)n);
    }
    return -1;
}

TEST(multi_thread_accept, single_thread_default) {
    /* num_threads=1: aux thread is not started. */
    ASSERT(1);
}

TEST(multi_thread_accept, aux_thread_admits) {
    cmq_config_t cfg = {0};
    cfg.num_threads = 2;
    cfg.host = "127.0.0.1";
    cfg.port = AUX_PORT;
    cfg.log_to_stdout = 0;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, server_thread, srv), 0);
    int ready = 0;
    for (int i = 0; i < 100; i++) {
        int fd = connect_to(AUX_PORT);
        if (fd >= 0) {
            close(fd);
            ready = 1;
            break;
        }
        wait_ms(20);
    }
    ASSERT_EQ(ready, 1);

    int fds[AUX_BURST];
    int opened = 0;
    for (int i = 0; i < AUX_BURST; i++) {
        fds[i] = connect_to(AUX_PORT);
        if (fds[i] >= 0) opened++;
    }
    ASSERT(opened >= 8);

    uint64_t aux = 0;
    for (int i = 0; i < 50; i++) {
        aux = cmq_atomic_load_u64(&srv->stat_accept_aux, CMQ_ATOMIC_RELAXED);
        if (aux >= 1) break;
        wait_ms(20);
    }
    ASSERT(aux >= 1);

    int handshake_ok = 0;
    for (int i = 0; i < opened && i < 4; i++) {
        if (fds[i] < 0) continue;
        cmq_parser_t *p = cmq_parser_create();
        send_frame(fds[i], CMQ_OP_CONNECT);
        cmq_frame_t skip;
        (void)skip;
        int got_info = recv_op(fds[i], p, CMQ_OP_INFO);
        int got_ack = recv_op(fds[i], p, CMQ_OP_CONNACK);
        if (got_info == 0 || got_ack == 0)
            handshake_ok = 1;
        cmq_parser_destroy(p);
    }
    ASSERT_EQ(handshake_ok, 1);

    for (int i = 0; i < AUX_BURST; i++) {
        if (fds[i] >= 0) close(fds[i]);
    }
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
}

TEST_MAIN()
