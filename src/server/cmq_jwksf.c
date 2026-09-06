#define _POSIX_C_SOURCE 200809L
#include "cmq_jwksf.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int host_ok(const char *h) {
    if (!h || !h[0]) return 0;
    for (const char *p = h; *p; p++) {
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-')
            continue;
        return 0;
    }
    return 1;
}

static int path_ok(const char *p) {
    if (!p || p[0] != '/') return 0;
    if (strstr(p, "..")) return 0;
    for (const char *s = p; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c == ' ' || c == '\\' || c == '#' || c == '@' ||
            c == '\r' || c == '\n')
            return 0;
    }
    return 1;
}

int cmq_jwks_parse_url(const char *url, cmq_jwks_url_t *out) {
    if (!url || !out) return -1;
    memset(out, 0, sizeof(*out));
    size_t n = strnlen(url, CMQ_JWKS_URL_MAX + 1);
    if (n == 0 || n > CMQ_JWKS_URL_MAX) return -1;
    if (strncmp(url, "http://", 7) != 0) return -1;
    const char *p = url + 7;
    if (!p[0] || p[0] == '/') return -1;
    if (strchr(p, '@')) return -1;
    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(hostend - p));
    size_t hlen = (size_t)((colon ? colon : hostend) - p);
    if (hlen == 0 || hlen >= sizeof(out->host)) return -1;
    memcpy(out->host, p, hlen);
    out->host[hlen] = '\0';
    if (!host_ok(out->host)) return -1;
    int port = CMQ_JWKS_DEFAULT_PORT;
    if (colon) {
        size_t plen = (size_t)(hostend - (colon + 1));
        if (plen == 0 || plen > 5) return -1;
        char pbuf[8];
        memcpy(pbuf, colon + 1, plen);
        pbuf[plen] = '\0';
        char *end = NULL;
        long v = strtol(pbuf, &end, 10);
        if (!end || *end || v < 1 || v > 65535) return -1;
        port = (int)v;
    }
    out->port = port;
    if (slash) {
        size_t plen = strlen(slash);
        if (plen >= sizeof(out->path)) return -1;
        memcpy(out->path, slash, plen + 1);
    } else {
        memcpy(out->path, "/.well-known/jwks.json", 23);
    }
    if (!path_ok(out->path)) return -1;
    return 0;
}

int cmq_jwks_build_get(const cmq_jwks_url_t *url, char *out, size_t cap) {
    if (!url || !url->host[0] || !url->path[0] || !out || cap == 0)
        return -1;
    int n = snprintf(out, cap,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s:%d\r\n"
                     "Accept: application/json\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     url->path, url->host, url->port);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int cmq_jwks_http_get(const cmq_jwks_url_t *url, cmq_jwks_t *out) {
    if (!url || !out) return -1;
    char req[512];
    if (cmq_jwks_build_get(url, req, sizeof(req)) < 0) return -1;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portbuf[8];
    snprintf(portbuf, sizeof(portbuf), "%d", url->port);
    struct addrinfo *res = NULL;
    if (getaddrinfo(url->host, portbuf, &hints, &res) != 0 || !res)
        return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    if (set_nonblock(fd) != 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    int cr = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (cr != 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
        close(fd);
        return -1;
    }
    struct pollfd pfd = {.fd = fd, .events = POLLOUT};
    if (poll(&pfd, 1, CMQ_JWKS_IO_MS) <= 0) {
        close(fd);
        return -1;
    }
    int so = 0;
    socklen_t sl = sizeof(so);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) != 0 || so != 0) {
        close(fd);
        return -1;
    }
    size_t off = 0, rlen = strlen(req);
    while (off < rlen) {
        pfd.events = POLLOUT;
        if (poll(&pfd, 1, CMQ_JWKS_IO_MS) <= 0) {
            close(fd);
            return -1;
        }
        ssize_t w = send(fd, req + off, rlen - off, 0);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            close(fd);
            return -1;
        }
        if (w == 0) {
            close(fd);
            return -1;
        }
        off += (size_t)w;
    }
    char resp[CMQ_JWKS_JSON_MAX + 512];
    size_t n = 0;
    while (n < sizeof(resp) - 1) {
        pfd.events = POLLIN;
        if (poll(&pfd, 1, CMQ_JWKS_IO_MS) <= 0) break;
        ssize_t r = recv(fd, resp + n, sizeof(resp) - 1 - n, 0);
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }
        if (r == 0) break;
        n += (size_t)r;
        if (n >= sizeof(resp) - 1) {
            close(fd);
            return -1;
        }
    }
    close(fd);
    resp[n] = '\0';
    if (n < 12 || strncmp(resp, "HTTP/1.", 7) != 0) return -1;
    const char *sp = strchr(resp, ' ');
    if (!sp || sp[1] != '2') return -1;
    char *body = strstr(resp, "\r\n\r\n");
    if (!body) return -1;
    body += 4;
    size_t blen = strlen(body);
    if (blen == 0 || blen > CMQ_JWKS_JSON_MAX) return -1;
    return cmq_jwks_parse(body, out);
}
