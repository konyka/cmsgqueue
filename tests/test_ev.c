#define _POSIX_C_SOURCE 200809L
#include "cmq_ev.h"
#include "cmq_test.h"
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <errno.h>

TEST(ev, create_destroy) {
    cmq_ev_loop_t *loop = cmq_ev_loop_create(64);
    ASSERT_NOT_NULL(loop);
    cmq_ev_loop_destroy(loop);
}

TEST(ev, add_del_fd) {
    cmq_ev_loop_t *loop = cmq_ev_loop_create(64);
    ASSERT_NOT_NULL(loop);

    int fds[2];
    int rc = pipe(fds);
    ASSERT_EQ(rc, 0);

    rc = cmq_ev_add(loop, fds[0], CMQ_EV_READ, NULL, NULL);
    ASSERT_EQ(rc, 0);

    rc = cmq_ev_del(loop, fds[0]);
    ASSERT_EQ(rc, 0);

    close(fds[0]);
    close(fds[1]);
    cmq_ev_loop_destroy(loop);
}

TEST(ev, mod_fd) {
    cmq_ev_loop_t *loop = cmq_ev_loop_create(64);
    ASSERT_NOT_NULL(loop);

    int fds[2];
    pipe(fds);

    cmq_ev_add(loop, fds[0], CMQ_EV_READ, NULL, NULL);
    int rc = cmq_ev_mod(loop, fds[0], CMQ_EV_READ | CMQ_EV_WRITE, NULL, NULL);
    ASSERT_EQ(rc, 0);

    cmq_ev_del(loop, fds[0]);
    close(fds[0]);
    close(fds[1]);
    cmq_ev_loop_destroy(loop);
}

TEST(ev, del_nonexistent) {
    cmq_ev_loop_t *loop = cmq_ev_loop_create(64);
    int rc = cmq_ev_del(loop, 999);
    ASSERT(rc != 0);
    cmq_ev_loop_destroy(loop);
}

static int ev_test_got_read = 0;
static cmq_ev_loop_t *read_test_loop = NULL;
static void ev_read_cb(int fd, int events, void *data) {
    (void)fd;
    (void)data;
    if (events & CMQ_EV_READ) ev_test_got_read = 1;
    if (read_test_loop) cmq_ev_stop(read_test_loop);
}

TEST(ev, readable_event) {
    cmq_ev_loop_t *loop = cmq_ev_loop_create(64);
    ev_test_got_read = 0;
    read_test_loop = loop;

    int fds[2];
    pipe(fds);

    cmq_ev_add(loop, fds[0], CMQ_EV_READ, ev_read_cb, NULL);

    write(fds[1], "x", 1);

    int rc = cmq_ev_run(loop, 100);
    ASSERT(rc >= 0);
    ASSERT_EQ(ev_test_got_read, 1);

    cmq_ev_del(loop, fds[0]);
    close(fds[0]);
    close(fds[1]);
    read_test_loop = NULL;
    cmq_ev_loop_destroy(loop);
}

static int ev_test_got_write = 0;
static cmq_ev_loop_t *write_test_loop = NULL;
static void ev_write_cb(int fd, int events, void *data) {
    (void)fd;
    (void)data;
    if (events & CMQ_EV_WRITE) ev_test_got_write = 1;
    if (write_test_loop) cmq_ev_stop(write_test_loop);
}

TEST(ev, writable_event) {
    cmq_ev_loop_t *loop = cmq_ev_loop_create(64);
    ev_test_got_write = 0;
    write_test_loop = loop;

    int fds[2];
    pipe(fds);

    cmq_ev_add(loop, fds[1], CMQ_EV_WRITE, ev_write_cb, NULL);

    int rc = cmq_ev_run(loop, 100);
    ASSERT(rc >= 0);
    ASSERT_EQ(ev_test_got_write, 1);

    cmq_ev_del(loop, fds[1]);
    close(fds[0]);
    close(fds[1]);
    write_test_loop = NULL;
    cmq_ev_loop_destroy(loop);
}

static int timer_fired = 0;
static cmq_ev_loop_t *timer_test_loop = NULL;
static void timer_cb(int fd, int events, void *data) {
    (void)fd;
    (void)events;
    (void)data;
    timer_fired = 1;
    if (timer_test_loop) cmq_ev_stop(timer_test_loop);
}

TEST(ev, timer_basic) {
    cmq_ev_loop_t *loop = cmq_ev_loop_create(64);
    timer_fired = 0;
    timer_test_loop = loop;

    int tid = cmq_ev_timer_add(loop, 10, 0, timer_cb, NULL);
    ASSERT(tid >= 0);

    cmq_ev_run(loop, -1);
    ASSERT_EQ(timer_fired, 1);

    timer_test_loop = NULL;
    cmq_ev_loop_destroy(loop);
}

TEST(ev, timer_cancel) {
    cmq_ev_loop_t *loop = cmq_ev_loop_create(64);
    timer_fired = 0;

    int tid = cmq_ev_timer_add(loop, 50, 0, timer_cb, NULL);
    ASSERT(tid >= 0);

    cmq_ev_timer_del(loop, tid);

    cmq_ev_run(loop, 20);
    ASSERT_EQ(timer_fired, 0);

    cmq_ev_loop_destroy(loop);
}

static int timer_count = 0;
static cmq_ev_loop_t *repeating_loop = NULL;
static void repeating_timer_cb(int fd, int events, void *data) {
    (void)fd;
    (void)events;
    (void)data;
    timer_count++;
    if (timer_count >= 3 && repeating_loop) {
        cmq_ev_stop(repeating_loop);
    }
}

TEST(ev, timer_repeating) {
    cmq_ev_loop_t *loop = cmq_ev_loop_create(64);
    timer_count = 0;
    repeating_loop = loop;

    int tid = cmq_ev_timer_add(loop, 10, 20, repeating_timer_cb, NULL);
    ASSERT(tid >= 0);

    cmq_ev_run(loop, -1);

    ASSERT(timer_count >= 3);

    cmq_ev_timer_del(loop, tid);
    repeating_loop = NULL;
    cmq_ev_loop_destroy(loop);
}

TEST(ev, stop) {
    cmq_ev_loop_t *loop = cmq_ev_loop_create(64);
    cmq_ev_stop(loop);
    int rc = cmq_ev_run(loop, 0);
    ASSERT(rc == 0);
    cmq_ev_loop_destroy(loop);
}

TEST(ev, fd_backend) {
    cmq_ev_loop_t *loop = cmq_ev_loop_create(64);
    int fd = cmq_ev_fd(loop);
    ASSERT(fd >= 0);
    cmq_ev_loop_destroy(loop);
}

TEST_MAIN()
