#define _POSIX_C_SOURCE 200809L
#include "cmq_otlp.h"

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

int cmq_otlp_parse_url(const char *url, cmq_otlp_url_t *out) {
    if (!url || !out) return -1;
    memset(out, 0, sizeof(*out));
    size_t n = strnlen(url, CMQ_OTLP_URL_MAX + 1);
    if (n == 0 || n > CMQ_OTLP_URL_MAX) return -1;
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
    int port = CMQ_OTLP_DEFAULT_PORT;
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
        memcpy(out->path, "/v1/traces", 11);
    }
    if (!path_ok(out->path)) return -1;
    return 0;
}

static void hex32(const uint8_t *in, size_t n, char *out) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = h[in[i] >> 4];
        out[i * 2 + 1] = h[in[i] & 0xF];
    }
    out[n * 2] = '\0';
}

static const char *kind_name(uint8_t k) {
    if (k == CMQ_OTEL_KIND_PUBLISH) return "publish";
    if (k == CMQ_OTEL_KIND_CONSUME) return "consume";
    if (k == CMQ_OTEL_KIND_CONNECT) return "connect";
    return "span";
}

static int kind_otlp(uint8_t k) {
    if (k == CMQ_OTEL_KIND_PUBLISH) return 4;
    if (k == CMQ_OTEL_KIND_CONSUME) return 5;
    return 1;
}

int cmq_otlp_encode_json(const cmq_otel_span_t *spans, size_t n,
                         char *out, size_t out_len) {
    if (!spans || !out || n == 0 || out_len < 64) return -1;
    int off = snprintf(out, out_len,
        "{\"resourceSpans\":[{\"resource\":{\"attributes\":["
        "{\"key\":\"service.name\",\"value\":{\"stringValue\":\"cmsgqueue\"}}]}"
        ",\"scopeSpans\":[{\"spans\":[");
    if (off < 0 || (size_t)off >= out_len) return -1;
    for (size_t i = 0; i < n; i++) {
        if (spans[i].kind < CMQ_OTEL_KIND_PUBLISH ||
            spans[i].kind > CMQ_OTEL_KIND_CONNECT)
            return -1;
        char tid[33], sid[17];
        hex32(spans[i].trace, 16, tid);
        hex32(spans[i].trace, 8, sid);
        uint64_t nano = spans[i].t_ms * 1000000ULL;
        int w = snprintf(out + off, out_len - (size_t)off,
            "%s{\"traceId\":\"%s\",\"spanId\":\"%s\",\"name\":\"%s\","
            "\"kind\":%d,\"startTimeUnixNano\":\"%llu\","
            "\"endTimeUnixNano\":\"%llu\"}",
            i ? "," : "", tid, sid, kind_name(spans[i].kind),
            kind_otlp(spans[i].kind),
            (unsigned long long)nano, (unsigned long long)nano);
        if (w < 0 || (size_t)w >= out_len - (size_t)off) return -1;
        off += w;
    }
    int w = snprintf(out + off, out_len - (size_t)off, "]}]}]}");
    if (w < 0 || (size_t)w >= out_len - (size_t)off) return -1;
    return off + w;
}

int cmq_otlp_build_request(const cmq_otlp_url_t *url, const char *body,
                           char *out, size_t out_len) {
    if (!url || !url->host[0] || !url->path[0] || !body || !out)
        return -1;
    size_t blen = strlen(body);
    int n = snprintf(out, out_len,
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        url->path, url->host, url->port, blen, body);
    if (n < 0 || (size_t)n >= out_len) return -1;
    return n;
}

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int cmq_otlp_http_post(const cmq_otlp_url_t *url, const char *body) {
    if (!url || !body) return -1;
    char req[CMQ_OTLP_JSON_MAX + 512];
    if (cmq_otlp_build_request(url, body, req, sizeof(req)) < 0)
        return -1;
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
    if (poll(&pfd, 1, 200) <= 0) {
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
        if (poll(&pfd, 1, 200) <= 0) {
            close(fd);
            return -1;
        }
        ssize_t w = write(fd, req + off, rlen - off);
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
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 200) <= 0) {
        close(fd);
        return -1;
    }
    char resp[128];
    ssize_t nr = read(fd, resp, sizeof(resp) - 1);
    close(fd);
    if (nr < 12) return -1;
    resp[nr] = '\0';
    if (strncmp(resp, "HTTP/1.", 7) != 0) return -1;
    const char *sp = strchr(resp, ' ');
    if (!sp || sp[1] != '2') return -1;
    return 0;
}

void cmq_otlp_export(void *ctx, const cmq_otel_span_t *span) {
    if (!ctx || !span) return;
    char json[CMQ_OTLP_JSON_MAX];
    if (cmq_otlp_encode_json(span, 1, json, sizeof(json)) < 0)
        return;
    (void)cmq_otlp_http_post((const cmq_otlp_url_t *)ctx, json);
}
