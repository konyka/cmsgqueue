#include "cmq_monitor.h"
#include <stdio.h>
#include <string.h>

int cmq_json_escape(const char *in, char *out, size_t cap) {
    if (!out || cap == 0) return -1;
    if (!in) in = "";
    size_t o = 0;
    for (size_t i = 0; in[i]; i++) {
        unsigned char c = (unsigned char)in[i];
        const char *rep = NULL;
        char hex[8];
        if (c == '"') rep = "\\\"";
        else if (c == '\\') rep = "\\\\";
        else if (c == '\n') rep = "\\n";
        else if (c == '\r') rep = "\\r";
        else if (c == '\t') rep = "\\t";
        else if (c < 0x20) {
            snprintf(hex, sizeof(hex), "\\u%04x", c);
            rep = hex;
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (o + rl + 1 > cap) return -1;
            memcpy(out + o, rep, rl);
            o += rl;
        } else {
            if (o + 2 > cap) return -1;
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return 0;
}

static int app(char *buf, size_t cap, size_t *pos, const char *s) {
    size_t n = strlen(s);
    if (*pos + n >= cap) return -1;
    memcpy(buf + *pos, s, n);
    *pos += n;
    buf[*pos] = '\0';
    return 0;
}

static int app_esc(char *buf, size_t cap, size_t *pos, const char *s) {
    char tmp[512];
    if (cmq_json_escape(s, tmp, sizeof(tmp)) != 0) return -1;
    return app(buf, cap, pos, tmp);
}

static const char *state_name(int s) {
    switch (s) {
    case 1: return "connected";
    case 2: return "closing";
    case 3: return "closed";
    default: return "init";
    }
}

int cmq_monitor_format_connz(char *buf, size_t cap,
                              const cmq_monitor_conn_t *conns, int n) {
    if (!buf || cap < 8) return -1;
    if (n < 0) n = 0;
    if (!conns) n = 0;
    size_t pos = 0;
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "{\"num_connections\":%d,\"connections\":[", n);
    if (app(buf, cap, &pos, hdr) != 0) return -1;
    int wrote = 0;
    for (int i = 0; i < n; i++) {
        size_t mark = pos;
        if (wrote && app(buf, cap, &pos, ",") != 0) {
            pos = mark;
            break;
        }
        char pre[128];
        snprintf(pre, sizeof(pre),
                 "{\"cid\":%u,\"state\":\"%s\",\"account\":\"",
                 (unsigned)conns[i].cid, state_name(conns[i].state));
        if (app(buf, cap, &pos, pre) != 0 ||
            app_esc(buf, cap, &pos, conns[i].account) != 0 ||
            app(buf, cap, &pos, "\",\"user\":\"") != 0 ||
            app_esc(buf, cap, &pos, conns[i].user) != 0)
            { pos = mark; break; }
        char mid[160];
        snprintf(mid, sizeof(mid),
                 "\",\"subs\":%d,\"ws\":%d,\"route\":%d,\"tid\":\"",
                 conns[i].nsubs, conns[i].ws ? 1 : 0,
                 conns[i].route ? 1 : 0);
        if (app(buf, cap, &pos, mid) != 0 ||
            app_esc(buf, cap, &pos, conns[i].tid) != 0 ||
            app(buf, cap, &pos, "\"}") != 0)
            { pos = mark; break; }
        wrote++;
    }
    if (app(buf, cap, &pos, "]") != 0) return -1;
    if (wrote < n && app(buf, cap, &pos, ",\"truncated\":1") != 0)
        return -1;
    if (app(buf, cap, &pos, "}\n") != 0) return -1;
    return (int)pos;
}

int cmq_monitor_format_subz(char *buf, size_t cap,
                             const cmq_monitor_sub_t *subs, int n) {
    if (!buf || cap < 8) return -1;
    if (n < 0) n = 0;
    if (!subs) n = 0;
    size_t pos = 0;
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "{\"num_subscriptions\":%d,\"subscriptions\":[", n);
    if (app(buf, cap, &pos, hdr) != 0) return -1;
    int wrote = 0;
    for (int i = 0; i < n; i++) {
        size_t mark = pos;
        if (wrote && app(buf, cap, &pos, ",") != 0) {
            pos = mark;
            break;
        }
        char pre[80];
        snprintf(pre, sizeof(pre), "{\"cid\":%u,\"sid\":%u,\"subject\":\"",
                 (unsigned)subs[i].cid, (unsigned)subs[i].sid);
        if (app(buf, cap, &pos, pre) != 0 ||
            app_esc(buf, cap, &pos, subs[i].subject) != 0 ||
            app(buf, cap, &pos, "\",\"queue\":\"") != 0 ||
            app_esc(buf, cap, &pos, subs[i].queue) != 0 ||
            app(buf, cap, &pos, "\"}") != 0)
            { pos = mark; break; }
        wrote++;
    }
    if (app(buf, cap, &pos, "]") != 0) return -1;
    if (wrote < n && app(buf, cap, &pos, ",\"truncated\":1") != 0)
        return -1;
    if (app(buf, cap, &pos, "}\n") != 0) return -1;
    return (int)pos;
}

int cmq_monitor_format_routez(char *buf, size_t cap,
                               int live, int held, int targets,
                               const cmq_monitor_route_t *routes, int n) {
    if (!buf || cap < 8) return -1;
    if (n < 0) n = 0;
    if (!routes) n = 0;
    size_t pos = 0;
    char hdr[128];
    snprintf(hdr, sizeof(hdr),
             "{\"num_routes\":%d,\"live\":%d,\"held\":%d,\"targets\":%d,\"routes\":[",
             n, live, held, targets);
    if (app(buf, cap, &pos, hdr) != 0) return -1;
    int wrote = 0;
    for (int i = 0; i < n; i++) {
        size_t mark = pos;
        if (wrote && app(buf, cap, &pos, ",") != 0) {
            pos = mark;
            break;
        }
        if (app(buf, cap, &pos, "{\"id\":\"") != 0 ||
            app_esc(buf, cap, &pos, routes[i].id) != 0 ||
            app(buf, cap, &pos, "\",\"addr\":\"") != 0 ||
            app_esc(buf, cap, &pos, routes[i].addr) != 0)
            { pos = mark; break; }
        char tail[80];
        snprintf(tail, sizeof(tail),
                 "\",\"port\":%d,\"connected\":%d,\"fd\":%d}",
                 routes[i].port, routes[i].connected ? 1 : 0,
                 routes[i].fd);
        if (app(buf, cap, &pos, tail) != 0) {
            pos = mark;
            break;
        }
        wrote++;
    }
    if (app(buf, cap, &pos, "]") != 0) return -1;
    if (wrote < n && app(buf, cap, &pos, ",\"truncated\":1") != 0)
        return -1;
    if (app(buf, cap, &pos, "}\n") != 0) return -1;
    return (int)pos;
}
