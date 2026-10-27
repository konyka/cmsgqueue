#ifndef CMQ_IO_H
#define CMQ_IO_H

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* write(2) with SIGPIPE held back for the duration of the call. If the write
 * fails with EPIPE and SIGPIPE became pending because of it, the signal is
 * consumed here, so the caller sees only the -1/EPIPE result. A SIGPIPE the
 * caller had already blocked is left alone. Used where send() has no
 * MSG_NOSIGNAL (macOS). Masking works for every descriptor no matter where
 * it was created, so no socket creation or accept site needs a per-socket
 * option. */
static inline ssize_t cmq_write_no_sigpipe(int fd, const void *buf, size_t len) {
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &block, &old) != 0)
        return write(fd, buf, len);

    ssize_t n = write(fd, buf, len);
    int err = errno;
    if (n < 0 && err == EPIPE && !sigismember(&old, SIGPIPE)) {
        sigset_t pending;
        sigemptyset(&pending);
        if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE)) {
            int sig = 0;
            (void)sigwait(&block, &sig);
        }
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    errno = err;
    return n;
}

/* write(2) on a socket raises SIGPIPE when the peer is gone, and the
 * default action terminates the whole process. A library must not change
 * the host's signal disposition, so socket writes go through send() with
 * MSG_NOSIGNAL where the platform has it, and through
 * cmq_write_no_sigpipe() where it does not. Descriptors that are not sockets
 * (pipes used by tests and tools) fall back to write(). */
static inline ssize_t cmq_sock_write(int fd, const void *buf, size_t len) {
#ifdef MSG_NOSIGNAL
    ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
    if (n < 0 && errno == ENOTSOCK)
        return write(fd, buf, len);
    return n;
#else
    return cmq_write_no_sigpipe(fd, buf, len);
#endif
}

#endif
