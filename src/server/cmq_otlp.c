#define _POSIX_C_SOURCE 200809L
#include "cmq_otlp.h"
#include "cmq_h2.h"
#include "cmq_hpack.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
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

int cmq_otlp_set_ca(cmq_otlp_url_t *url, const char *ca_path) {
    if (!url || !ca_path || !ca_path[0]) return -1;
    size_t n = strnlen(ca_path, CMQ_OTLP_CA_MAX);
    if (n == 0 || n >= CMQ_OTLP_CA_MAX) return -1;
    if (strstr(ca_path, "..")) return -1;
    memcpy(url->ca, ca_path, n + 1);
    return 0;
}

int cmq_otlp_parse_url(const char *url, cmq_otlp_url_t *out) {
    if (!url || !out) return -1;
    memset(out, 0, sizeof(*out));
    size_t n = strnlen(url, CMQ_OTLP_URL_MAX + 1);
    if (n == 0 || n > CMQ_OTLP_URL_MAX) return -1;
    const char *p;
    if (strncmp(url, "https://", 8) == 0) {
        out->tls = 1;
        p = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        p = url + 7;
    } else if (strncmp(url, "grpc://", 7) == 0) {
        out->grpc = 1;
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
    int port = out->grpc ? CMQ_OTLP_GRPC_PORT : CMQ_OTLP_DEFAULT_PORT;
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
    } else if (out->grpc) {
        memcpy(out->path, CMQ_OTLP_GRPC_PATH, sizeof(CMQ_OTLP_GRPC_PATH));
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
    if (k == CMQ_OTEL_KIND_REQUEST) return "request";
    if (k == CMQ_OTEL_KIND_RESPONSE) return "response";
    return "span";
}

static int kind_otlp(uint8_t k) {
    if (k == CMQ_OTEL_KIND_PUBLISH) return 4;
    if (k == CMQ_OTEL_KIND_CONSUME) return 5;
    if (k == CMQ_OTEL_KIND_REQUEST) return 2;
    if (k == CMQ_OTEL_KIND_RESPONSE) return 4;
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
            spans[i].kind > CMQ_OTEL_KIND_RESPONSE)
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

static int set_block_timeout(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    if (fcntl(fd, F_SETFL, fl & ~O_NONBLOCK) != 0) return -1;
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = CMQ_OTLP_IO_MS * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0)
        return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0)
        return -1;
    return 0;
}

static int otlp_tls_io(int fd, const cmq_otlp_url_t *url, const char *req,
                       char *resp, size_t resp_cap, ssize_t *nread) {
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
            int nr = SSL_read(ssl, resp, (int)resp_cap - 1);
            if (nr >= 12) {
                resp[nr] = '\0';
                *nread = nr;
                rc = 0;
            }
        }
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    return rc;
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
    if (poll(&pfd, 1, CMQ_OTLP_IO_MS) <= 0) {
        close(fd);
        return -1;
    }
    int so = 0;
    socklen_t sl = sizeof(so);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) != 0 || so != 0) {
        close(fd);
        return -1;
    }
    char resp[128];
    ssize_t nr = 0;
    if (url->tls) {
        if (otlp_tls_io(fd, url, req, resp, sizeof(resp), &nr) != 0) {
            close(fd);
            return -1;
        }
        close(fd);
    } else {
        size_t off = 0, rlen = strlen(req);
        while (off < rlen) {
            pfd.events = POLLOUT;
            if (poll(&pfd, 1, CMQ_OTLP_IO_MS) <= 0) {
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
        if (poll(&pfd, 1, CMQ_OTLP_IO_MS) <= 0) {
            close(fd);
            return -1;
        }
        nr = read(fd, resp, sizeof(resp) - 1);
        close(fd);
    }
    if (nr < 12) return -1;
    resp[nr] = '\0';
    if (strncmp(resp, "HTTP/1.", 7) != 0) return -1;
    const char *sp = strchr(resp, ' ');
    if (!sp || sp[1] != '2') return -1;
    return 0;
}

static int pb_varint(uint8_t *o, size_t cap, size_t *off, uint64_t v) {
    do {
        if (*off >= cap) return -1;
        uint8_t b = (uint8_t)(v & 0x7f);
        v >>= 7;
        if (v) b |= 0x80;
        o[(*off)++] = b;
    } while (v);
    return 0;
}

static int pb_key(uint8_t *o, size_t cap, size_t *off, uint32_t field, int wt) {
    return pb_varint(o, cap, off, ((uint64_t)field << 3) | (uint64_t)wt);
}

static int pb_bytes(uint8_t *o, size_t cap, size_t *off, uint32_t field,
                    const void *p, size_t n) {
    if (pb_key(o, cap, off, field, 2) != 0) return -1;
    if (pb_varint(o, cap, off, n) != 0) return -1;
    if (*off + n > cap) return -1;
    memcpy(o + *off, p, n);
    *off += n;
    return 0;
}

static int pb_fixed64(uint8_t *o, size_t cap, size_t *off, uint32_t field,
                      uint64_t v) {
    if (pb_key(o, cap, off, field, 1) != 0) return -1;
    if (*off + 8 > cap) return -1;
    for (int i = 0; i < 8; i++)
        o[(*off)++] = (uint8_t)((v >> (8 * i)) & 0xff);
    return 0;
}

static int pb_varint_field(uint8_t *o, size_t cap, size_t *off, uint32_t field,
                           uint64_t v) {
    if (pb_key(o, cap, off, field, 0) != 0) return -1;
    return pb_varint(o, cap, off, v);
}

static int pb_embed(uint8_t *o, size_t cap, size_t *off, uint32_t field,
                    const uint8_t *inner, size_t n) {
    return pb_bytes(o, cap, off, field, inner, n);
}

int cmq_otlp_encode_proto(const cmq_otel_span_t *spans, size_t n,
                          uint8_t *out, size_t out_len) {
    if (!spans || !out || n == 0 || out_len < 16) return -1;
    uint8_t kv[64], any[32], res[96], scope[256], rs[384];
    size_t kvn = 0, anyn = 0, resn = 0, scopen = 0, rsn = 0;
    if (pb_bytes(any, sizeof(any), &anyn, 1, "cmsgqueue", 9) != 0)
        return -1;
    if (pb_bytes(kv, sizeof(kv), &kvn, 1, "service.name", 12) != 0)
        return -1;
    if (pb_embed(kv, sizeof(kv), &kvn, 2, any, anyn) != 0) return -1;
    if (pb_embed(res, sizeof(res), &resn, 1, kv, kvn) != 0) return -1;
    for (size_t i = 0; i < n; i++) {
        if (spans[i].kind < CMQ_OTEL_KIND_PUBLISH ||
            spans[i].kind > CMQ_OTEL_KIND_RESPONSE)
            return -1;
        uint8_t one[192];
        size_t on = 0;
        if (pb_bytes(one, sizeof(one), &on, 1, spans[i].trace, 16) != 0)
            return -1;
        if (pb_bytes(one, sizeof(one), &on, 2, spans[i].trace, 8) != 0)
            return -1;
        const char *nm = kind_name(spans[i].kind);
        if (pb_bytes(one, sizeof(one), &on, 5, nm, strlen(nm)) != 0)
            return -1;
        if (pb_varint_field(one, sizeof(one), &on, 6,
                            (uint64_t)kind_otlp(spans[i].kind)) != 0)
            return -1;
        uint64_t nano = spans[i].t_ms * 1000000ULL;
        if (pb_fixed64(one, sizeof(one), &on, 7, nano) != 0) return -1;
        if (pb_fixed64(one, sizeof(one), &on, 8, nano) != 0) return -1;
        if (pb_embed(scope, sizeof(scope), &scopen, 2, one, on) != 0)
            return -1;
    }
    if (pb_embed(rs, sizeof(rs), &rsn, 1, res, resn) != 0) return -1;
    if (pb_embed(rs, sizeof(rs), &rsn, 2, scope, scopen) != 0) return -1;
    size_t off = 0;
    if (pb_embed(out, out_len, &off, 1, rs, rsn) != 0) return -1;
    return (int)off;
}

int cmq_otlp_grpc_frame(const uint8_t *proto, size_t plen,
                        uint8_t *out, size_t out_len) {
    if (!proto || !out || plen == 0 || plen > 0xffffffu) return -1;
    if (out_len < plen + 5) return -1;
    out[0] = 0;
    out[1] = (uint8_t)((plen >> 24) & 0xff);
    out[2] = (uint8_t)((plen >> 16) & 0xff);
    out[3] = (uint8_t)((plen >> 8) & 0xff);
    out[4] = (uint8_t)(plen & 0xff);
    memcpy(out + 5, proto, plen);
    return (int)(plen + 5);
}

static void h2_hdr(uint8_t *b, uint32_t len, uint8_t type, uint8_t flags,
                   uint32_t sid) {
    b[0] = (uint8_t)((len >> 16) & 0xff);
    b[1] = (uint8_t)((len >> 8) & 0xff);
    b[2] = (uint8_t)(len & 0xff);
    b[3] = type;
    b[4] = flags;
    b[5] = (uint8_t)((sid >> 24) & 0x7f);
    b[6] = (uint8_t)((sid >> 16) & 0xff);
    b[7] = (uint8_t)((sid >> 8) & 0xff);
    b[8] = (uint8_t)(sid & 0xff);
}

static int otlp_h2_req(const cmq_otlp_url_t *url, const uint8_t *grpc,
                       size_t glen, uint8_t *out, size_t cap) {
    uint8_t blk[384];
    size_t bo = 0;
    blk[bo++] = 0x83;
    int n = cmq_hpack_int_encode(4, 6, 0x40, blk + bo, sizeof(blk) - bo);
    if (n < 0) return -1;
    bo += (size_t)n;
    n = cmq_hpack_str_encode(url->path, blk + bo, sizeof(blk) - bo);
    if (n < 0) return -1;
    bo += (size_t)n;
    blk[bo++] = 0x86;
    n = cmq_hpack_int_encode(1, 6, 0x40, blk + bo, sizeof(blk) - bo);
    if (n < 0) return -1;
    bo += (size_t)n;
    char auth[160];
    int an = snprintf(auth, sizeof(auth), "%s:%d", url->host, url->port);
    if (an < 0 || (size_t)an >= sizeof(auth)) return -1;
    n = cmq_hpack_str_encode(auth, blk + bo, sizeof(blk) - bo);
    if (n < 0) return -1;
    bo += (size_t)n;
    blk[bo++] = 0x40;
    n = cmq_hpack_str_encode("content-type", blk + bo, sizeof(blk) - bo);
    if (n < 0) return -1;
    bo += (size_t)n;
    n = cmq_hpack_str_encode("application/grpc", blk + bo, sizeof(blk) - bo);
    if (n < 0) return -1;
    bo += (size_t)n;
    size_t need = CMQ_H2_PREFACE_LEN + 15 + 9 + bo + 9 + glen;
    if (need > cap) return -1;
    size_t o = 0;
    memcpy(out + o, cmq_h2_preface, CMQ_H2_PREFACE_LEN);
    o += CMQ_H2_PREFACE_LEN;
    uint8_t setpl[6];
    if (cmq_h2_settings_encode(32, setpl, sizeof(setpl)) != 6) return -1;
    h2_hdr(out + o, 6, CMQ_H2_TYPE_SETTINGS, 0, 0);
    memcpy(out + o + 9, setpl, 6);
    o += 15;
    h2_hdr(out + o, (uint32_t)bo, CMQ_H2_TYPE_HEADERS, 0x04, 1);
    memcpy(out + o + 9, blk, bo);
    o += 9 + bo;
    h2_hdr(out + o, (uint32_t)glen, CMQ_H2_TYPE_DATA, 0x01, 1);
    memcpy(out + o + 9, grpc, glen);
    o += 9 + glen;
    return (int)o;
}

int cmq_otlp_grpc_post(const cmq_otlp_url_t *url, const cmq_otel_span_t *span) {
    if (!url || !url->grpc || !span) return -1;
    uint8_t proto[CMQ_OTLP_JSON_MAX];
    int pn = cmq_otlp_encode_proto(span, 1, proto, sizeof(proto));
    if (pn < 0) return -1;
    uint8_t frame[CMQ_OTLP_JSON_MAX + 8];
    int fn = cmq_otlp_grpc_frame(proto, (size_t)pn, frame, sizeof(frame));
    if (fn < 0) return -1;
    uint8_t req[CMQ_OTLP_JSON_MAX + 768];
    int rn = otlp_h2_req(url, frame, (size_t)fn, req, sizeof(req));
    if (rn < 0) return -1;
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
    if (poll(&pfd, 1, CMQ_OTLP_IO_MS) <= 0) {
        close(fd);
        return -1;
    }
    int so = 0;
    socklen_t sl = sizeof(so);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) != 0 || so != 0) {
        close(fd);
        return -1;
    }
    size_t off = 0;
    while (off < (size_t)rn) {
        pfd.events = POLLOUT;
        if (poll(&pfd, 1, CMQ_OTLP_IO_MS) <= 0) {
            close(fd);
            return -1;
        }
        ssize_t w = send(fd, req + off, (size_t)rn - off, 0);
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
    close(fd);
    return 0;
}

void cmq_otlp_export(void *ctx, const cmq_otel_span_t *span) {
    if (!ctx || !span) return;
    const cmq_otlp_url_t *u = ctx;
    if (u->grpc) {
        (void)cmq_otlp_grpc_post(u, span);
        return;
    }
    char json[CMQ_OTLP_JSON_MAX];
    if (cmq_otlp_encode_json(span, 1, json, sizeof(json)) < 0)
        return;
    (void)cmq_otlp_http_post(u, json);
}
