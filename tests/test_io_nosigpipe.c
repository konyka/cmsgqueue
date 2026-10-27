#define _POSIX_C_SOURCE 200809L
#include "cmq_test.h"
#include "cmq_io.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int sigpipe_pending(void) {
    sigset_t pending;
    sigemptyset(&pending);
    sigpending(&pending);
    return sigismember(&pending, SIGPIPE);
}

/* SIGPIPE is blocked by the callers, so a raised one stays pending. Consume
 * it with sigwait (which returns at once for a pending signal) rather than
 * the Linux/POSIX-realtime sigtimedwait, which macOS does not have. */
static void drain_sigpipe(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    while (sigpipe_pending()) {
        int sig = 0;
        if (sigwait(&set, &sig) != 0) break;
    }
}

static volatile sig_atomic_t g_sigpipe_count;

static void count_sigpipe(int sig) {
    (void)sig;
    g_sigpipe_count++;
}

typedef ssize_t (*write_fn)(int, const void *, size_t);

static ssize_t sock_write_fn(int fd, const void *buf, size_t len) {
    return cmq_sock_write(fd, buf, len);
}

/* Writes to a socket whose peer is closed, with SIGPIPE unblocked and a
 * counting handler installed, so a raised signal cannot go unnoticed. */
static void write_to_dead_peer(write_fn fn, ssize_t *n, int *err, int *count,
                               int *blocked_after) {
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = count_sigpipe;
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(sigaction(SIGPIPE, &sa, &old_sa), 0);

    sigset_t unblock, old;
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGPIPE);
    ASSERT_EQ(pthread_sigmask(SIG_UNBLOCK, &unblock, &old), 0);
    drain_sigpipe();
    g_sigpipe_count = 0;

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    close(sv[1]);

    errno = 0;
    *n = fn(sv[0], "x", 1);
    *err = errno;
    *count = (int)g_sigpipe_count;
    sigset_t cur;
    pthread_sigmask(SIG_BLOCK, NULL, &cur);
    *blocked_after = sigismember(&cur, SIGPIPE);
    close(sv[0]);

    pthread_sigmask(SIG_SETMASK, &old, NULL);
    sigaction(SIGPIPE, &old_sa, NULL);
}

TEST(io_nosigpipe, write_to_closed_socket_peer_reports_epipe_without_signal) {
    ssize_t n; int err, count, blocked;
    write_to_dead_peer(sock_write_fn, &n, &err, &count, &blocked);
    ASSERT_EQ(n, -1);
    ASSERT_EQ(err, EPIPE);
    ASSERT_EQ(count, 0);
    ASSERT_EQ(blocked, 0);
}

TEST(io_nosigpipe, masked_write_fallback_reports_epipe_without_signal) {
    ssize_t n; int err, count, blocked;
    write_to_dead_peer(cmq_write_no_sigpipe, &n, &err, &count, &blocked);
    ASSERT_EQ(n, -1);
    ASSERT_EQ(err, EPIPE);
    ASSERT_EQ(count, 0);
    ASSERT_EQ(blocked, 0);
}

TEST(io_nosigpipe, masked_write_fallback_leaves_a_caller_blocked_signal_alone) {
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    ASSERT_EQ(pthread_sigmask(SIG_BLOCK, &block, &old), 0);
    drain_sigpipe();

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    close(sv[1]);
    ssize_t n = cmq_write_no_sigpipe(sv[0], "x", 1);
    int pending = sigpipe_pending();
    close(sv[0]);
    drain_sigpipe();
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    ASSERT_EQ(n, -1);
    ASSERT_EQ(pending, 1);
}

TEST(io_nosigpipe, unguarded_write_raises_sigpipe_when_unblocked) {
    ssize_t n; int err, count, blocked;
    write_to_dead_peer(write, &n, &err, &count, &blocked);
    ASSERT_EQ(n, -1);
    ASSERT_EQ(err, EPIPE);
    ASSERT_EQ(count, 1);
}

TEST(io_nosigpipe, plain_write_would_raise_sigpipe) {
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    ASSERT_EQ(pthread_sigmask(SIG_BLOCK, &block, &old), 0);
    drain_sigpipe();

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    close(sv[1]);
    ssize_t n = write(sv[0], "x", 1);
    int pending = sigpipe_pending();
    close(sv[0]);
    drain_sigpipe();
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    ASSERT_EQ(n, -1);
    ASSERT_EQ(pending, 1);
}

TEST(io_nosigpipe, non_socket_descriptor_falls_back_to_write) {
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    ASSERT_EQ(cmq_sock_write(fds[1], "ab", 2), 2);
    char buf[4] = {0};
    ASSERT_EQ(read(fds[0], buf, sizeof(buf)), 2);
    ASSERT_EQ(memcmp(buf, "ab", 2), 0);
    close(fds[0]);
    close(fds[1]);
}

TEST(io_nosigpipe, delivers_bytes_on_a_healthy_socket) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    ASSERT_EQ(cmq_sock_write(sv[0], "hello", 5), 5);
    char buf[8] = {0};
    ASSERT_EQ(read(sv[1], buf, sizeof(buf)), 5);
    ASSERT_EQ(memcmp(buf, "hello", 5), 0);
    close(sv[0]);
    close(sv[1]);
}

TEST_MAIN()
