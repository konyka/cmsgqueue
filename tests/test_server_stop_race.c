#define _POSIX_C_SOURCE 200809L
#include "cmq_test.h"
#include "cmq_server.h"

#include <pthread.h>
#include <sched.h>
#include <signal.h>
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

TEST_MAIN()
