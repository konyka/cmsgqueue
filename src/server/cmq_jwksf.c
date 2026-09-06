#define _POSIX_C_SOURCE 200809L
#include "cmq_jwksf.h"
#include "cmq_thread.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
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

int cmq_jwks_set_ca(cmq_jwks_url_t *url, const char *ca_path) {
    if (!url || !ca_path || !ca_path[0]) return -1;
    size_t n = strnlen(ca_path, CMQ_JWKS_CA_MAX);
    if (n == 0 || n >= CMQ_JWKS_CA_MAX) return -1;
    if (strstr(ca_path, "..")) return -1;
    memcpy(url->ca, ca_path, n + 1);
    return 0;
}

int cmq_jwks_parse_url(const char *url, cmq_jwks_url_t *out) {
    if (!url || !out) return -1;
    memset(out, 0, sizeof(*out));
    size_t n = strnlen(url, CMQ_JWKS_URL_MAX + 1);
    if (n == 0 || n > CMQ_JWKS_URL_MAX) return -1;
    const char *p;
    if (strncmp(url, "https://", 8) == 0) {
        out->tls = 1;
        p = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        p = url + 7;
    } else {
        return -1;
    }
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
    int port = out->tls ? CMQ_JWKS_TLS_PORT : CMQ_JWKS_DEFAULT_PORT;
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

static int set_block_timeout(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    if (fcntl(fd, F_SETFL, fl & ~O_NONBLOCK) != 0) return -1;
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = CMQ_JWKS_IO_MS * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0)
        return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0)
        return -1;
    return 0;
}

static int jwks_tls_io(int fd, const cmq_jwks_url_t *url, const char *req,
                       char *resp, size_t resp_cap, size_t *nread) {
    if (set_block_timeout(fd) != 0) return -1;
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return -1;
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    int vrc;
    if (url->ca[0])
        vrc = SSL_CTX_load_verify_locations(ctx, url->ca, NULL);
    else
        vrc = SSL_CTX_set_default_verify_paths(ctx);
    if (vrc != 1) {
        SSL_CTX_free(ctx);
        return -1;
    }
    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        SSL_CTX_free(ctx);
        return -1;
    }
    SSL_set_fd(ssl, fd);
    (void)SSL_set_tlsext_host_name(ssl, url->host);
    if (inet_addr(url->host) != (in_addr_t)INADDR_NONE) {
        X509_VERIFY_PARAM *pm = SSL_get0_param(ssl);
        if (pm)
            (void)X509_VERIFY_PARAM_set1_ip_asc(pm, url->host);
    } else {
        (void)SSL_set1_host(ssl, url->host);
    }
    int rc = -1;
    if (SSL_connect(ssl) == 1) {
        size_t off = 0, rlen = strlen(req);
        int ok = 1;
        while (off < rlen) {
            int w = SSL_write(ssl, req + off, (int)(rlen - off));
            if (w <= 0) {
                ok = 0;
                break;
            }
            off += (size_t)w;
        }
        if (ok) {
            size_t n = 0;
            while (n < resp_cap - 1) {
                int r = SSL_read(ssl, resp + n, (int)(resp_cap - 1 - n));
                if (r <= 0) break;
                n += (size_t)r;
                if (n >= resp_cap - 1) {
                    n = 0;
                    break;
                }
            }
            if (n > 0) {
                resp[n] = '\0';
                *nread = n;
                rc = 0;
            }
        }
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    return rc;
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
    char resp[CMQ_JWKS_JSON_MAX + 512];
    size_t n = 0;
    if (url->tls) {
        if (jwks_tls_io(fd, url, req, resp, sizeof(resp), &n) != 0) {
            close(fd);
            return -1;
        }
        close(fd);
    } else {
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
    }
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

struct cmq_jwks_cache {
    cmq_jwks_t slot[2];
    atomic_int idx;
    atomic_int have;
};

struct cmq_jwks_refresher {
    cmq_jwks_url_t url;
    cmq_jwks_cache_t *cache;
    atomic_uint interval_sec;
    uint64_t last_ms;
    atomic_int run;
    cmq_thread_t thr;
    int started;
};

static uint64_t jwks_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

cmq_jwks_cache_t *cmq_jwks_cache_create(void) {
    cmq_jwks_cache_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    atomic_init(&c->idx, 0);
    atomic_init(&c->have, 0);
    return c;
}

void cmq_jwks_cache_destroy(cmq_jwks_cache_t *c) {
    free(c);
}

int cmq_jwks_cache_put(cmq_jwks_cache_t *c, const cmq_jwks_t *src) {
    if (!c || !src || src->n <= 0) return -1;
    int have = atomic_load_explicit(&c->have, memory_order_acquire);
    int cur = atomic_load_explicit(&c->idx, memory_order_relaxed);
    int idle = have ? (1 - (cur & 1)) : 0;
    c->slot[idle] = *src;
    atomic_store_explicit(&c->idx, idle, memory_order_release);
    atomic_store_explicit(&c->have, 1, memory_order_release);
    return 0;
}

const cmq_jwks_t *cmq_jwks_cache_get(const cmq_jwks_cache_t *c) {
    if (!c) return NULL;
    if (!atomic_load_explicit(&c->have, memory_order_acquire))
        return NULL;
    int i = atomic_load_explicit(&c->idx, memory_order_acquire);
    return &c->slot[i & 1];
}

int cmq_jwks_cache_reload(cmq_jwks_cache_t **cache, const char **live_json,
                          const char *fresh_json) {
    if (!cache) return -1;
    if (!fresh_json || !fresh_json[0])
        return 0;

    cmq_jwks_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (cmq_jwks_parse(fresh_json, &tmp) != 0)
        return -1;

    char *owned = strdup(fresh_json);
    if (!owned) return -1;

    cmq_jwks_cache_t *c = *cache;
    int created = 0;
    if (!c) {
        c = cmq_jwks_cache_create();
        if (!c) {
            free(owned);
            return -1;
        }
        created = 1;
    }
    if (cmq_jwks_cache_put(c, &tmp) != 0) {
        if (created) cmq_jwks_cache_destroy(c);
        free(owned);
        return -1;
    }
    if (created) *cache = c;
    if (live_json) {
        free((void *)*live_json);
        *live_json = owned;
    } else {
        free(owned);
    }
    return 0;
}

int cmq_jwks_refresh_due(uint64_t last_ms, uint64_t now_ms,
                         unsigned interval_sec) {
    if (interval_sec == 0) return 0;
    uint64_t span = (uint64_t)interval_sec * 1000ULL;
    if (now_ms < last_ms) return 1;
    return (now_ms - last_ms) >= span ? 1 : 0;
}

int cmq_jwks_refresh_step(const cmq_jwks_url_t *url, cmq_jwks_cache_t *cache,
                          uint64_t *last_ms, uint64_t now_ms,
                          unsigned interval_sec) {
    if (!url || !cache || !last_ms) return -1;
    if (!cmq_jwks_refresh_due(*last_ms, now_ms, interval_sec))
        return 0;
    cmq_jwks_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (cmq_jwks_http_get(url, &tmp) != 0)
        return -1;
    if (cmq_jwks_cache_put(cache, &tmp) != 0)
        return -1;
    *last_ms = now_ms;
    return 0;
}

static void *jwks_refresh_thr(void *arg) {
    cmq_jwks_refresher_t *r = arg;
    while (atomic_load_explicit(&r->run, memory_order_acquire)) {
        uint64_t now = jwks_now_ms();
        unsigned iv = atomic_load_explicit(&r->interval_sec,
                                           memory_order_acquire);
        (void)cmq_jwks_refresh_step(&r->url, r->cache, &r->last_ms, now, iv);
        for (int i = 0; i < 10 &&
             atomic_load_explicit(&r->run, memory_order_acquire); i++) {
            struct timespec ts = {0, 100000000L};
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

cmq_jwks_refresher_t *cmq_jwks_refresh_start(const cmq_jwks_url_t *url,
                                             cmq_jwks_cache_t *cache,
                                             unsigned interval_sec) {
    if (!url || !url->host[0] || !cache || interval_sec == 0)
        return NULL;
    cmq_jwks_refresher_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->url = *url;
    r->cache = cache;
    atomic_init(&r->interval_sec, interval_sec);
    r->last_ms = jwks_now_ms();
    atomic_init(&r->run, 1);
    if (cmq_thread_create(&r->thr, jwks_refresh_thr, r) != 0) {
        free(r);
        return NULL;
    }
    r->started = 1;
    return r;
}

void cmq_jwks_refresh_stop(cmq_jwks_refresher_t *r) {
    if (!r) return;
    if (r->started) {
        atomic_store_explicit(&r->run, 0, memory_order_release);
        cmq_thread_join(r->thr);
        r->started = 0;
    }
    free(r);
}

unsigned cmq_jwks_refresh_interval(cmq_jwks_refresher_t *r) {
    if (!r) return 0;
    return atomic_load_explicit(&r->interval_sec, memory_order_acquire);
}

int cmq_jwks_refresh_reload(cmq_jwks_refresher_t *r, int *live_sec,
                            int fresh_sec) {
    if (!live_sec) return -1;
    if (fresh_sec < 0 || fresh_sec > CMQ_JWKS_REFRESH_MAX)
        return -1;
    if (fresh_sec != 0 && fresh_sec < CMQ_JWKS_REFRESH_MIN)
        return -1;
    if (fresh_sec == 0)
        return 0;
    if (r)
        atomic_store_explicit(&r->interval_sec, (unsigned)fresh_sec,
                              memory_order_release);
    *live_sec = fresh_sec;
    return 0;
}
