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

TEST(io_nosigpipe, write_to_closed_socket_peer_reports_epipe_without_signal) {
#ifndef MSG_NOSIGNAL
    /* cmq_sock_write only avoids SIGPIPE through send(MSG_NOSIGNAL). Where
     * that flag does not exist (macOS) it falls back to write(), which can
     * raise the signal, so this guarantee does not hold there yet. Skip
     * instead of passing falsely; see the CHANGELOG entry on macOS SIGPIPE. */
    fprintf(stderr, "SKIP: MSG_NOSIGNAL not available on this platform\n");
    return;
#endif
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    ASSERT_EQ(pthread_sigmask(SIG_BLOCK, &block, &old), 0);
    drain_sigpipe();

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    close(sv[1]);

    errno = 0;
    ssize_t n = cmq_sock_write(sv[0], "x", 1);
    int err = errno;
    int pending = sigpipe_pending();
    close(sv[0]);
    drain_sigpipe();
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    ASSERT_EQ(n, -1);
    ASSERT_EQ(err, EPIPE);
    ASSERT_EQ(pending, 0);
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
