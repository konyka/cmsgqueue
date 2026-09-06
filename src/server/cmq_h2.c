#define _POSIX_C_SOURCE 200809L
#include "cmq_h2.h"
#include "cmq_hpack.h"
#include "cmq_tls.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

const uint8_t cmq_h2_preface[CMQ_H2_PREFACE_LEN] = {
    'P','R','I',' ','*',' ','H','T','T','P','/','2','.','0','\r','\n',
    '\r','\n','S','M','\r','\n','\r','\n'
};

struct cmq_h2 {
    int state;
    int streams;
    uint8_t used[CMQ_H2_MAX_STREAMS];
    uint32_t ids[CMQ_H2_MAX_STREAMS];
};

cmq_h2_t *cmq_h2_create(void) {
    cmq_h2_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->state = CMQ_H2_ST_PREFACE;
    return h;
}

void cmq_h2_destroy(cmq_h2_t *h) {
    free(h);
}

int cmq_h2_preface_ok(const uint8_t *p, size_t n) {
    if (!p || n != CMQ_H2_PREFACE_LEN) return -1;
    return memcmp(p, cmq_h2_preface, CMQ_H2_PREFACE_LEN) == 0 ? 0 : -1;
}

int cmq_h2_frame_hdr(const uint8_t *in, size_t n, uint32_t *len,
                     uint8_t *type, uint8_t *flags, uint32_t *sid) {
    if (!in || n < CMQ_H2_HDR_LEN || !len || !type || !flags || !sid)
        return -1;
    *len = ((uint32_t)in[0] << 16) | ((uint32_t)in[1] << 8) | in[2];
    *type = in[3];
    *flags = in[4];
    *sid = ((uint32_t)(in[5] & 0x7f) << 24) | ((uint32_t)in[6] << 16) |
           ((uint32_t)in[7] << 8) | in[8];
    return 0;
}

int cmq_h2_settings_encode(uint32_t max_streams, uint8_t *out, size_t cap) {
    if (!out || cap < 6 || max_streams == 0 ||
        max_streams > CMQ_H2_MAX_STREAMS)
        return -1;
    out[0] = 0;
    out[1] = CMQ_H2_SETTINGS_MAX_CONCURRENT_STREAMS;
    out[2] = (uint8_t)((max_streams >> 24) & 0xff);
    out[3] = (uint8_t)((max_streams >> 16) & 0xff);
    out[4] = (uint8_t)((max_streams >> 8) & 0xff);
    out[5] = (uint8_t)(max_streams & 0xff);
    return 6;
}

int cmq_h2_settings_decode(const uint8_t *in, size_t n, uint32_t *max_streams) {
    if (!in || !max_streams || n < 6 || (n % 6) != 0) return -1;
    int found = 0;
    for (size_t i = 0; i < n; i += 6) {
        uint16_t id = ((uint16_t)in[i] << 8) | in[i + 1];
        uint32_t v = ((uint32_t)in[i + 2] << 24) | ((uint32_t)in[i + 3] << 16) |
                     ((uint32_t)in[i + 4] << 8) | in[i + 5];
        if (id == CMQ_H2_SETTINGS_MAX_CONCURRENT_STREAMS) {
            if (v == 0 || v > CMQ_H2_MAX_STREAMS) return -1;
            *max_streams = v;
            found = 1;
        }
    }
    return found ? 0 : -1;
}

static int h2_fail(cmq_h2_t *h) {
    h->state = CMQ_H2_ST_GOAWAY;
    return -1;
}

static int h2_find(cmq_h2_t *h, uint32_t sid) {
    for (int i = 0; i < h->streams; i++) {
        if (h->ids[i] == sid) return i;
    }
    return -1;
}

static int h2_open(cmq_h2_t *h, uint32_t sid) {
    if ((sid & 1u) == 0) return -1;
    if (h2_find(h, sid) >= 0) return 0;
    if (h->streams >= CMQ_H2_MAX_STREAMS) return -1;
    h->ids[h->streams] = sid;
    h->used[h->streams] = 1;
    h->streams++;
    return 0;
}

int cmq_h2_feed(cmq_h2_t *h, const uint8_t *data, size_t n, size_t *used) {
    if (!h || !data || !used) return -1;
    *used = 0;
    if (h->state == CMQ_H2_ST_GOAWAY) return -1;
    if (h->state == CMQ_H2_ST_PREFACE) {
        if (n < CMQ_H2_PREFACE_LEN) return 0;
        if (cmq_h2_preface_ok(data, CMQ_H2_PREFACE_LEN) != 0)
            return h2_fail(h);
        h->state = CMQ_H2_ST_SETTINGS;
        *used = CMQ_H2_PREFACE_LEN;
        return 1;
    }
    if (n < CMQ_H2_HDR_LEN) return 0;
    uint32_t len = 0, sid = 0;
    uint8_t type = 0, flags = 0;
    if (cmq_h2_frame_hdr(data, n, &len, &type, &flags, &sid) != 0)
        return h2_fail(h);
    if (len > CMQ_H2_MAX_FRAME) return h2_fail(h);
    if (n < CMQ_H2_HDR_LEN + len) return 0;
    const uint8_t *pl = data + CMQ_H2_HDR_LEN;
    if (h->state == CMQ_H2_ST_SETTINGS) {
        if (type != CMQ_H2_TYPE_SETTINGS || sid != 0 ||
            (flags & CMQ_H2_FLAG_ACK))
            return h2_fail(h);
        uint32_t ms = CMQ_H2_MAX_STREAMS;
        if (len > 0 && cmq_h2_settings_decode(pl, len, &ms) != 0)
            return h2_fail(h);
        h->state = CMQ_H2_ST_OPEN;
        *used = CMQ_H2_HDR_LEN + len;
        return 1;
    }
    if (type == CMQ_H2_TYPE_GOAWAY) {
        *used = CMQ_H2_HDR_LEN + len;
        return h2_fail(h);
    }
    if (type == CMQ_H2_TYPE_SETTINGS) {
        if (sid != 0) return h2_fail(h);
        *used = CMQ_H2_HDR_LEN + len;
        return 1;
    }
    if (type == CMQ_H2_TYPE_HEADERS) {
        if (h2_open(h, sid) != 0) return h2_fail(h);
        *used = CMQ_H2_HDR_LEN + len;
        return 1;
    }
    if (type == CMQ_H2_TYPE_DATA) {
        if (h2_find(h, sid) < 0) return h2_fail(h);
        *used = CMQ_H2_HDR_LEN + len;
        return 1;
    }
    return h2_fail(h);
}

int cmq_h2_state(const cmq_h2_t *h) {
    return h ? h->state : CMQ_H2_ST_GOAWAY;
}

int cmq_h2_stream_count(const cmq_h2_t *h) {
    return h ? h->streams : 0;
}

static int h2_loopback_ok(const char *bind_addr) {
    if (!bind_addr || bind_addr[0] == '\0') return 1;
    return strcmp(bind_addr, "127.0.0.1") == 0;
}

int cmq_h2_listen(const char *bind_addr, int port) {
    if (!h2_loopback_ok(bind_addr) || port < 0 || port > 65535) return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(s, 8) != 0) {
        close(s);
        return -1;
    }
    return s;
}

int cmq_h2_listen_port(int lfd) {
    if (lfd < 0) return -1;
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    if (getsockname(lfd, (struct sockaddr *)&addr, &alen) != 0) return -1;
    return (int)ntohs(addr.sin_port);
}

int cmq_h2_reload_listen(int *lfd, int *live_port, int fresh_port) {
    if (!lfd || !live_port) return -1;
    if (fresh_port < 0 || fresh_port > 65535) return -1;
    if (fresh_port == 0)
        return 0;
    if (*lfd >= 0)
        return 0;
    int s = cmq_h2_listen("127.0.0.1", fresh_port);
    if (s < 0) return -1;
    *lfd = s;
    *live_port = fresh_port;
    return 0;
}

int cmq_listener_reload_bind(int *lfd, const char **live_host, int *live_port,
                             const char *fresh_host, int fresh_port,
                             int default_port) {
    if (!lfd || !live_port) return -1;
    if (fresh_port < 0 || fresh_port > 65535) return -1;
    if (default_port < 0 || default_port > 65535) return -1;
    if ((!fresh_host || !fresh_host[0]) && fresh_port == 0 &&
        default_port == 0)
        return 0;
    if (*lfd >= 0)
        return 0;
    const char *use_host = (fresh_host && fresh_host[0])
                               ? fresh_host
                               : (live_host && *live_host && (*live_host)[0]
                                      ? *live_host
                                      : "127.0.0.1");
    int use_port = fresh_port > 0 ? fresh_port
                                  : (*live_port > 0 ? *live_port : default_port);
    if (use_port <= 0)
        return 0;
    struct in_addr ha;
    if (inet_pton(AF_INET, use_host, &ha) != 1)
        return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)use_port);
    addr.sin_addr = ha;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(s, 512) != 0) {
        close(s);
        return -1;
    }
    if (fresh_host && fresh_host[0] && live_host) {
        char *owned = strdup(fresh_host);
        if (!owned) {
            close(s);
            return -1;
        }
        free((void *)*live_host);
        *live_host = owned;
    }
    *lfd = s;
    *live_port = use_port;
    return 0;
}

typedef struct {
    ssize_t (*rd)(void *ctx, uint8_t *buf, size_t n);
    ssize_t (*wr)(void *ctx, const uint8_t *buf, size_t n);
    int (*wait)(void *ctx, int want_write);
    void *ctx;
} h2_io_t;

static int h2_write_all(const h2_io_t *io, const uint8_t *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        if (io->wait(io->ctx, 1) != 0) return -1;
        ssize_t w = io->wr(io->ctx, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return -1;
        }
        if (w == 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

static void h2_write_goaway(const h2_io_t *io) {
    uint8_t f[17];
    memset(f, 0, sizeof(f));
    f[2] = 8;
    f[3] = CMQ_H2_TYPE_GOAWAY;
    (void)h2_write_all(io, f, sizeof(f));
}

static int h2_write_settings_ok(const h2_io_t *io) {
    uint8_t ack[9];
    memset(ack, 0, sizeof(ack));
    ack[3] = CMQ_H2_TYPE_SETTINGS;
    ack[4] = CMQ_H2_FLAG_ACK;
    if (h2_write_all(io, ack, sizeof(ack)) != 0) return -1;
    uint8_t fr[15];
    memset(fr, 0, sizeof(fr));
    fr[2] = 6;
    fr[3] = CMQ_H2_TYPE_SETTINGS;
    if (cmq_h2_settings_encode(CMQ_H2_MAX_STREAMS, fr + 9, 6) != 6) return -1;
    return h2_write_all(io, fr, sizeof(fr));
}

static int h2_path_subject(const char *path, char *out, size_t cap) {
    if (!path || path[0] != '/' || !out || cap == 0) return -1;
    const char *s = path + 1;
    size_t n = strlen(s);
    if (n == 0 || n >= cap || n > CMQ_H2_SUBJECT_MAX) return -1;
    if (strstr(s, "..")) return -1;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '*' || c == '>' || c == ' ' || c == '/' || c == '\\')
            return -1;
    }
    memcpy(out, s, n + 1);
    return 0;
}

static int h2_headers_take(cmq_hpack_dyn_t *dyn, const uint8_t *pl, size_t len,
                           char *method, size_t mcap, char *path,
                           size_t pcap) {
    method[0] = '\0';
    path[0] = '\0';
    size_t off = 0;
    while (off < len) {
        char name[CMQ_HPACK_STR_MAX + 1];
        char value[CMQ_HPACK_STR_MAX + 1];
        size_t used = 0;
        int r = cmq_hpack_hdr_decode_dyn(dyn, pl + off, len - off, name,
                                         sizeof(name), value, sizeof(value),
                                         &used);
        if (r < 0 || used == 0 || used > len - off) return -1;
        off += used;
        if (r == 1) continue;
        if (strcmp(name, ":method") == 0) {
            if (strlen(value) >= mcap) return -1;
            memcpy(method, value, strlen(value) + 1);
        } else if (strcmp(name, ":path") == 0) {
            if (strlen(value) >= pcap) return -1;
            memcpy(path, value, strlen(value) + 1);
        }
    }
    return 0;
}

static int h2_io_read(const h2_io_t *io, uint8_t *buf, size_t cap) {
    if (io->wait(io->ctx, 0) != 0) return -1;
    ssize_t n = io->rd(io->ctx, buf, cap);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    if (n == 0) return -1;
    return (int)n;
}

static int h2_session_io(const h2_io_t *io, char *subject, size_t scap,
                         uint8_t *payload, size_t pcap, size_t *plen) {
    if (!io || !io->rd || !io->wr || !io->wait || !subject || scap == 0 ||
        !payload || pcap == 0 || !plen)
        return -1;
    *plen = 0;
    subject[0] = '\0';
    cmq_h2_t *h = cmq_h2_create();
    if (!h) return -1;
    cmq_hpack_dyn_t dyn;
    cmq_hpack_dyn_init(&dyn);
    uint8_t buf[CMQ_H2_MAX_FRAME + 64];
    size_t blen = 0;
    int got_path = 0;
    int sent_set = 0;
    int steps = 0;
    while (steps++ < 64) {
        if (blen < 9) {
            int n = h2_io_read(io, buf + blen, sizeof(buf) - blen);
            if (n < 0) {
                h2_write_goaway(io);
                cmq_h2_destroy(h);
                return -1;
            }
            if (n == 0) continue;
            blen += (size_t)n;
            continue;
        }
        size_t used = 0;
        int before = cmq_h2_state(h);
        int r = cmq_h2_feed(h, buf, blen, &used);
        if (r < 0) {
            h2_write_goaway(io);
            cmq_h2_destroy(h);
            return -1;
        }
        if (r == 0) {
            if (blen >= sizeof(buf)) {
                h2_write_goaway(io);
                cmq_h2_destroy(h);
                return -1;
            }
            int n = h2_io_read(io, buf + blen, sizeof(buf) - blen);
            if (n < 0) {
                h2_write_goaway(io);
                cmq_h2_destroy(h);
                return -1;
            }
            if (n == 0) continue;
            blen += (size_t)n;
            continue;
        }
        if (before == CMQ_H2_ST_PREFACE) {
            memmove(buf, buf + used, blen - used);
            blen -= used;
            continue;
        }
        uint8_t type = 0;
        uint32_t flen = 0;
        if (used >= CMQ_H2_HDR_LEN) {
            type = buf[3];
            flen = ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2];
        }
        if (type == CMQ_H2_TYPE_SETTINGS && !sent_set &&
            (buf[4] & CMQ_H2_FLAG_ACK) == 0) {
            if (h2_write_settings_ok(io) != 0) {
                h2_write_goaway(io);
                cmq_h2_destroy(h);
                return -1;
            }
            sent_set = 1;
        }
        if (type == CMQ_H2_TYPE_HEADERS && used >= CMQ_H2_HDR_LEN) {
            char method[16];
            char path[CMQ_H2_SUBJECT_MAX + 2];
            if (h2_headers_take(&dyn, buf + CMQ_H2_HDR_LEN, flen, method,
                                sizeof(method), path, sizeof(path)) != 0 ||
                strcmp(method, "POST") != 0 ||
                h2_path_subject(path, subject, scap) != 0) {
                h2_write_goaway(io);
                cmq_h2_destroy(h);
                return -1;
            }
            got_path = 1;
            if (buf[4] & CMQ_H2_FLAG_END_STREAM) {
                *plen = 0;
                memmove(buf, buf + used, blen - used);
                blen -= used;
                cmq_h2_destroy(h);
                return 0;
            }
        }
        if (type == CMQ_H2_TYPE_DATA && got_path && used >= CMQ_H2_HDR_LEN) {
            if (flen > pcap) {
                h2_write_goaway(io);
                cmq_h2_destroy(h);
                return -1;
            }
            memcpy(payload, buf + CMQ_H2_HDR_LEN, flen);
            *plen = flen;
            cmq_h2_destroy(h);
            return 0;
        }
        memmove(buf, buf + used, blen - used);
        blen -= used;
    }
    h2_write_goaway(io);
    cmq_h2_destroy(h);
    return -1;
}

static ssize_t h2_fd_rd(void *ctx, uint8_t *buf, size_t n) {
    return recv(*(int *)ctx, buf, n, 0);
}

static ssize_t h2_fd_wr(void *ctx, const uint8_t *buf, size_t n) {
    return send(*(int *)ctx, buf, n, 0);
}

static int h2_fd_wait(void *ctx, int want_write) {
    struct pollfd pfd = {
        .fd = *(int *)ctx,
        .events = want_write ? POLLOUT : POLLIN
    };
    return poll(&pfd, 1, CMQ_H2_IO_MS) <= 0 ? -1 : 0;
}

int cmq_h2_session(int fd, char *subject, size_t scap, uint8_t *payload,
                   size_t pcap, size_t *plen) {
    if (fd < 0) return -1;
    h2_io_t io = {h2_fd_rd, h2_fd_wr, h2_fd_wait, &fd};
    return h2_session_io(&io, subject, scap, payload, pcap, plen);
}

static ssize_t h2_tls_rd(void *ctx, uint8_t *buf, size_t n) {
    return cmq_tls_read((cmq_tls_session_t *)ctx, buf, n);
}

static ssize_t h2_tls_wr(void *ctx, const uint8_t *buf, size_t n) {
    return cmq_tls_write((cmq_tls_session_t *)ctx, buf, n);
}

static int h2_tls_wait(void *ctx, int want_write) {
    int fd = cmq_tls_fd((cmq_tls_session_t *)ctx);
    if (fd < 0) return -1;
    struct pollfd pfd = {.fd = fd, .events = want_write ? POLLOUT : POLLIN};
    return poll(&pfd, 1, CMQ_H2_IO_MS) <= 0 ? -1 : 0;
}

static int h2_tls_ready(cmq_tls_session_t *tls) {
    if (!tls) return -1;
    int fd = cmq_tls_fd(tls);
    if (fd < 0) return -1;
    for (int i = 0; i < 64; i++) {
        int rc = cmq_tls_handshake(tls);
        if (rc == 1) return 0;
        if (rc < 0) return -1;
        /* POLLOUT is almost always ready; wait for the peer. */
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, CMQ_H2_IO_MS);
        if (pr < 0 && errno != EINTR) return -1;
    }
    return -1;
}

int cmq_h2_session_tls(cmq_tls_session_t *tls, char *subject, size_t scap,
                       uint8_t *payload, size_t pcap, size_t *plen) {
    if (h2_tls_ready(tls) != 0) return -1;
    h2_io_t io = {h2_tls_rd, h2_tls_wr, h2_tls_wait, tls};
    return h2_session_io(&io, subject, scap, payload, pcap, plen);
}

int cmq_h2_accept(int lfd, char *subject, size_t scap, uint8_t *payload,
                  size_t pcap, size_t *plen) {
    if (lfd < 0 || !subject || !payload || !plen) return -1;
    struct pollfd pfd = {.fd = lfd, .events = POLLIN};
    if (poll(&pfd, 1, CMQ_H2_IO_MS) <= 0) return -1;
    int c = accept(lfd, NULL, NULL);
    if (c < 0) return -1;
    int r = cmq_h2_session(c, subject, scap, payload, pcap, plen);
    close(c);
    return r;
}

int cmq_h2_accept_tls(int lfd, cmq_tls_config_t *cfg, char *subject,
                      size_t scap, uint8_t *payload, size_t pcap,
                      size_t *plen) {
    if (lfd < 0 || !cfg || !subject || !payload || !plen) return -1;
    struct pollfd pfd = {.fd = lfd, .events = POLLIN};
    if (poll(&pfd, 1, CMQ_H2_IO_MS) <= 0) return -1;
    int c = accept(lfd, NULL, NULL);
    if (c < 0) return -1;
    cmq_tls_session_t *tls = cmq_tls_server_session(cfg, c);
    if (!tls) {
        close(c);
        return -1;
    }
    int r = cmq_h2_session_tls(tls, subject, scap, payload, pcap, plen);
    cmq_tls_session_destroy(tls);
    close(c);
    return r;
}
