#define _POSIX_C_SOURCE 200809L
#include "cmq_test.h"
#include "cmq_server.h"

#include <pthread.h>
#include <sched.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define STOP_RACE_PORT 19840

static void *run_thread(void *arg) {
    cmq_server_run((cmq_server_t *)arg);
    return NULL;
}

/* cmq_server_stop may be called from another thread while cmq_server_run is
 * still starting up. Stop right after run begins, with varying delays, so
 * stop's close/clear of the listener state overlaps run's setup. Under TSAN
 * any unsynchronised access is reported as a data race. A stop that arrives
 * before run has created its event loop must not be lost: run would then
 * block forever, so a watchdog turns that hang into a failure. */
TEST(server_stop_race, stop_while_run_is_starting) {
    alarm(60);
    for (int round = 0; round < 12; round++) {
        cmq_config_t cfg = {0};
        cfg.num_threads = 1;
        cfg.host = "127.0.0.1";
        cfg.port = STOP_RACE_PORT + (round % 8);
        cfg.log_to_stdout = 0;
        cmq_server_t *srv = NULL;
        ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

        pthread_t tid;
        ASSERT_EQ(pthread_create(&tid, NULL, run_thread, srv), 0);
        for (int spin = 0; spin < (round % 5) * 200; spin++)
            sched_yield();
        cmq_server_stop(srv);
        pthread_join(tid, NULL);
        cmq_server_destroy(srv);
    }
    alarm(0);
}

/* With two or more threads the server also runs an aux accept thread that
 * keeps the listener in its own epoll set. stop() must not close a listener
 * that thread can still touch (CI's TSAN reported close() in stop() against
 * epoll_ctl() in the aux thread, and a closed fd number can be reused by an
 * unrelated socket). This exercises that path; the CI race depends on timing
 * and could not be reproduced here, so a pass is not proof it is gone. */
static int wait_until_accepting(uint16_t port) {
    for (int i = 0; i < 400; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        int rc = connect(fd, (struct sockaddr *)&a, sizeof(a));
        close(fd);
        if (rc == 0) return 0;
        struct timespec ts = {0, 5000000};
        nanosleep(&ts, NULL);
    }
    return -1;
}

TEST(server_stop_race, stop_with_aux_accept_thread_running) {
    alarm(120);
    for (int round = 0; round < 6; round++) {
        cmq_config_t cfg = {0};
        cfg.num_threads = 2;
        cfg.host = "127.0.0.1";
        cfg.port = (uint16_t)(STOP_RACE_PORT + 16 + (round % 4));
        cfg.log_to_stdout = 0;
        cmq_server_t *srv = NULL;
        ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

        pthread_t tid;
        ASSERT_EQ(pthread_create(&tid, NULL, run_thread, srv), 0);
        ASSERT_EQ(wait_until_accepting(cfg.port), 0);
        cmq_server_stop(srv);
        pthread_join(tid, NULL);
        cmq_server_destroy(srv);
    }
    alarm(0);
}

TEST_MAIN()
