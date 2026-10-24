#ifndef CMQ_IO_H
#define CMQ_IO_H

#include <errno.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* write(2) on a socket raises SIGPIPE when the peer is gone, and the
 * default action terminates the whole process. A library must not change
 * the host's signal disposition, so socket writes go through send() with
 * MSG_NOSIGNAL where the platform has it. Descriptors that are not sockets
 * (pipes used by tests and tools) fall back to write(). */
static inline ssize_t cmq_sock_write(int fd, const void *buf, size_t len) {
#ifdef MSG_NOSIGNAL
    ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
    if (n < 0 && errno == ENOTSOCK)
        return write(fd, buf, len);
    return n;
#else
    return write(fd, buf, len);
#endif
}

#endif
