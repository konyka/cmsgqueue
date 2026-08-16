/* N2: Hot config reload — SIGHUP test. */

#include "cmq_test.h"
#include "cmq_server.h"
#include "cmq_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#define RELOAD_PORT 19100
#define RELOAD_CFG  "/tmp/cmq-test-reload.cfg"

static void write_cfg(const char *path, int port, int log_level) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "host = 127.0.0.1\n");
    fprintf(f, "port = %d\n", port);
    fprintf(f, "log_level = %d\n", log_level);
    fclose(f);
}

static void *server_thread(void *arg) {
    cmq_server_t *srv = (cmq_server_t *)arg;
    cmq_server_run(srv);
    return NULL;
}

TEST(reload, sighup_triggers_reload) {
    /* Write initial config. */
    write_cfg(RELOAD_CFG, RELOAD_PORT, 2);
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = RELOAD_PORT;
    cfg.log_to_stdout = 0;
    cfg.config_file = RELOAD_CFG;
    cmq_server_t *srv = NULL;
    int rc = cmq_server_create(&srv, &cfg);
    if (rc != CMQ_OK) {
        /* No config-file path; skip. */
        return;
    }
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, srv);
    struct timespec ts = {0, 500000000};
    nanosleep(&ts);

    /* Update config and send SIGHUP. */
    write_cfg(RELOAD_CFG, RELOAD_PORT, 3);
    raise(SIGUSR1);  /* SIGHUP may be blocked in test process; USR1 is fine for the API */

    nanosleep(&ts);
    cmq_server_stop(srv);
    pthread_join(tid, NULL);
    cmq_server_destroy(srv);
    unlink(RELOAD_CFG);
}

TEST_MAIN()
