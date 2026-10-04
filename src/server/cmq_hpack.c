#define _POSIX_C_SOURCE 200809L
#include "cmq_hpack.h"

#include <string.h>

typedef struct {
    const char *name;
    const char *value;
} cmq_hpack_static_t;

/* RFC 7541 Appendix A. Index 1..61. */
static const cmq_hpack_static_t cmq_hpack_static[CMQ_HPACK_STATIC_MAX + 1] = {
    {NULL, NULL},
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
};

int cmq_hpack_int_encode(uint32_t value, unsigned nbits, uint8_t prefix_hi,
                         uint8_t *out, size_t cap) {
    if (!out || nbits < 1 || nbits > 8 || value > CMQ_HPACK_INT_MAX)
        return -1;
    uint32_t maxp = (1u << nbits) - 1u;
    if ((prefix_hi & (uint8_t)maxp) != 0) return -1;
    if (cap < 1) return -1;
    if (value < maxp) {
        out[0] = (uint8_t)(prefix_hi | (uint8_t)value);
        return 1;
    }
    out[0] = (uint8_t)(prefix_hi | (uint8_t)maxp);
    value -= maxp;
    size_t n = 1;
    while (value >= 128) {
        if (n >= cap) return -1;
        out[n++] = (uint8_t)((value % 128) | 0x80);
        value /= 128;
    }
    if (n >= cap) return -1;
    out[n++] = (uint8_t)value;
    return (int)n;
}

int cmq_hpack_int_decode(const uint8_t *in, size_t in_len, unsigned nbits,
                         uint32_t *value, size_t *consumed) {
    if (!in || !value || nbits < 1 || nbits > 8 || in_len == 0)
        return -1;
    uint32_t maxp = (1u << nbits) - 1u;
    uint32_t v = (uint32_t)(in[0] & (uint8_t)maxp);
    size_t n = 1;
    if (v < maxp) {
        *value = v;
        if (consumed) *consumed = 1;
        return 0;
    }
    uint32_t m = 0;
    while (n < in_len) {
        uint8_t b = in[n++];
        uint32_t add = (uint32_t)(b & 0x7f) << m;
        if (v > CMQ_HPACK_INT_MAX - add) return -1;
        v += add;
        m += 7;
        if (m > 28) return -1;
        if ((b & 0x80) == 0) {
            if (v > CMQ_HPACK_INT_MAX) return -1;
            *value = v;
            if (consumed) *consumed = n;
            return 0;
        }
    }
    return -1;
}

/* RFC 7541 Appendix B. */
static const uint32_t cmq_huff_code[256] = {
    0x1ff8, 0x7fffd8, 0xfffffe2, 0xfffffe3, 0xfffffe4, 0xfffffe5, 0xfffffe6,
    0xfffffe7, 0xfffffe8, 0xffffea, 0x3ffffffc, 0xfffffe9, 0xfffffea,
    0x3ffffffd, 0xfffffeb, 0xfffffec, 0xfffffed, 0xfffffee, 0xfffffef,
    0xffffff0, 0xffffff1, 0xffffff2, 0x3ffffffe, 0xffffff3, 0xffffff4,
    0xffffff5, 0xffffff6, 0xffffff7, 0xffffff8, 0xffffff9, 0xffffffa,
    0xffffffb, 0x14, 0x3f8, 0x3f9, 0xffa, 0x1ff9, 0x15, 0xf8, 0x7fa,
    0x3fa, 0x3fb, 0xf9, 0x7fb, 0xfa, 0x16, 0x17, 0x18, 0x0, 0x1, 0x2,
    0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x5c, 0xfb, 0x7ffc, 0x20,
    0xffb, 0x3fc, 0x1ffa, 0x21, 0x5d, 0x5e, 0x5f, 0x60, 0x61, 0x62, 0x63,
    0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e,
    0x6f, 0x70, 0x71, 0x72, 0xfc, 0x73, 0xfd, 0x1ffb, 0x7fff0, 0x1ffc,
    0x3ffc, 0x22, 0x7ffd, 0x3, 0x23, 0x4, 0x24, 0x5, 0x25, 0x26, 0x27,
    0x6, 0x74, 0x75, 0x28, 0x29, 0x2a, 0x7, 0x2b, 0x76, 0x2c, 0x8, 0x9,
    0x2d, 0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7ffe, 0x7fc, 0x3ffd, 0x1ffd,
    0xffffffc, 0xfffe6, 0x3fffd2, 0xfffe7, 0xfffe8, 0x3fffd3, 0x3fffd4,
    0x3fffd5, 0x7fffd9, 0x3fffd6, 0x7fffda, 0x7fffdb, 0x7fffdc, 0x7fffdd,
    0x7fffde, 0xffffeb, 0x7fffdf, 0xffffec, 0xffffed, 0x3fffd7, 0x7fffe0,
    0xffffee, 0x7fffe1, 0x7fffe2, 0x7fffe3, 0x7fffe4, 0x1fffdc, 0x3fffd8,
    0x7fffe5, 0x3fffd9, 0x7fffe6, 0x7fffe7, 0xffffef, 0x3fffda, 0x1fffdd,
    0xfffe9, 0x3fffdb, 0x3fffdc, 0x7fffe8, 0x7fffe9, 0x1fffde, 0x7fffea,
    0x3fffdd, 0x3fffde, 0xfffff0, 0x1fffdf, 0x3fffdf, 0x7fffeb, 0x7fffec,
    0x1fffe0, 0x1fffe1, 0x3fffe0, 0x1fffe2, 0x7fffed, 0x3fffe1, 0x7fffee,
    0x7fffef, 0xfffea, 0x3fffe2, 0x3fffe3, 0x3fffe4, 0x7ffff0, 0x3fffe5,
    0x3fffe6, 0x7ffff1, 0x3ffffe0, 0x3ffffe1, 0xfffeb, 0x7fff1, 0x3fffe7,
    0x7ffff2, 0x3fffe8, 0x1ffffec, 0x3ffffe2, 0x3ffffe3, 0x3ffffe4,
    0x7ffffde, 0x7ffffdf, 0x3ffffe5, 0xfffff1, 0x1ffffed, 0x7fff2, 0x1fffe3,
    0x3ffffe6, 0x7ffffe0, 0x7ffffe1, 0x3ffffe7, 0x7ffffe2, 0xfffff2,
    0x1fffe4, 0x1fffe5, 0x3ffffe8, 0x3ffffe9, 0xffffffd, 0x7ffffe3,
    0x7ffffe4, 0x7ffffe5, 0xfffec, 0xfffff3, 0xfffed, 0x1fffe6, 0x3fffe9,
    0x1fffe7, 0x1fffe8, 0x7ffff3, 0x3fffea, 0x3fffeb, 0x1ffffee, 0x1ffffef,
    0xfffff4, 0xfffff5, 0x3ffffea, 0x7ffff4, 0x3ffffeb, 0x7ffffe6, 0x3ffffec,
    0x3ffffed, 0x7ffffe7, 0x7ffffe8, 0x7ffffe9, 0x7ffffea, 0x7ffffeb,
    0xffffffe, 0x7ffffec, 0x7ffffed, 0x7ffffee, 0x7ffffef, 0x7fffff0,
    0x3ffffee
};

static const uint8_t cmq_huff_bits[256] = {
    13, 23, 28, 28, 28, 28, 28, 28, 28, 24, 30, 28, 28, 30, 28, 28,
    28, 28, 28, 28, 28, 28, 30, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    6, 10, 10, 12, 13, 6, 8, 11, 10, 10, 8, 11, 8, 6, 6, 6,
    5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 7, 8, 15, 6, 12, 10,
    13, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 8, 7, 8, 13, 19, 13, 14, 6,
    15, 5, 6, 5, 6, 5, 6, 6, 6, 5, 7, 7, 6, 6, 6, 5,
    6, 7, 6, 5, 5, 6, 7, 7, 7, 7, 7, 15, 11, 14, 13, 28,
    20, 22, 20, 20, 22, 22, 22, 23, 22, 23, 23, 23, 23, 23, 24, 23,
    24, 24, 22, 23, 24, 23, 23, 23, 23, 21, 22, 23, 22, 23, 23, 24,
    22, 21, 20, 22, 22, 23, 23, 21, 23, 22, 22, 24, 21, 22, 23, 23,
    21, 21, 22, 21, 23, 22, 23, 23, 20, 22, 22, 22, 23, 22, 22, 23,
    26, 26, 20, 19, 22, 23, 22, 25, 26, 26, 26, 27, 27, 26, 24, 25,
    19, 21, 26, 27, 27, 26, 27, 24, 21, 21, 26, 26, 28, 27, 27, 27,
    20, 24, 20, 21, 22, 21, 21, 23, 22, 22, 25, 25, 24, 24, 26, 23,
    26, 27, 26, 26, 27, 27, 27, 27, 27, 28, 27, 27, 27, 27, 27, 26
};

int cmq_hpack_huff_encode(const uint8_t *in, size_t in_len, uint8_t *out,
                          size_t cap) {
    if (!in || !out || in_len == 0 || in_len > CMQ_HPACK_STR_MAX) return -1;
    uint64_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < in_len; i++) {
        uint8_t c = in[i];
        uint32_t code = cmq_huff_code[c];
        int len = cmq_huff_bits[c];
        acc = (acc << len) | code;
        bits += len;
        while (bits >= 8) {
            bits -= 8;
            if (n >= cap) return -1;
            out[n++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    if (bits > 0) {
        int pad = 8 - bits;
        acc = (acc << pad) | ((1u << pad) - 1u);
        if (n >= cap) return -1;
        out[n++] = (uint8_t)(acc & 0xff);
    }
    return (int)n;
}

int cmq_hpack_huff_decode(const uint8_t *in, size_t in_len, char *out,
                          size_t cap) {
    if (!in || !out || in_len == 0 || cap == 0) return -1;
    uint64_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < in_len; i++) {
        acc = (acc << 8) | in[i];
        bits += 8;
        int progress = 1;
        while (progress) {
            progress = 0;
            for (int s = 0; s < 256; s++) {
                int n = cmq_huff_bits[s];
                if (bits < n) continue;
                uint32_t got = (uint32_t)(acc >> (bits - n));
                uint32_t mask = n == 32 ? 0xffffffffu : ((1u << n) - 1u);
                if ((got & mask) == cmq_huff_code[s]) {
                    if (o + 1 >= cap) return -1;
                    out[o++] = (char)s;
                    bits -= n;
                    acc &= (bits == 0) ? 0 : ((1ull << bits) - 1ull);
                    progress = 1;
                    break;
                }
            }
        }
    }
    if (bits > 7) return -1;
    if (bits > 0) {
        uint32_t pad = (uint32_t)acc & ((1u << bits) - 1u);
        if (pad != ((1u << bits) - 1u)) return -1;
    }
    if (o == 0) return -1;
    out[o] = '\0';
    return (int)o;
}

int cmq_hpack_str_encode(const char *s, uint8_t *out, size_t cap) {
    if (!s || !out) return -1;
    size_t sl = strlen(s);
    if (sl == 0 || sl > CMQ_HPACK_STR_MAX) return -1;
    int n = cmq_hpack_int_encode((uint32_t)sl, 7, 0, out, cap);
    if (n < 0) return -1;
    if ((size_t)n + sl > cap) return -1;
    memcpy(out + n, s, sl);
    return n + (int)sl;
}

int cmq_hpack_str_decode(const uint8_t *in, size_t in_len, char *out,
                         size_t out_cap, size_t *consumed) {
    if (!in || !out || in_len == 0 || out_cap == 0) return -1;
    uint32_t sl = 0;
    size_t used = 0;
    int huff = (in[0] & 0x80) ? 1 : 0;
    if (cmq_hpack_int_decode(in, in_len, 7, &sl, &used) != 0) return -1;
    if (sl == 0 || sl > CMQ_HPACK_STR_MAX) return -1;
    if (used + sl > in_len) return -1;
    if (huff) {
        int d = cmq_hpack_huff_decode(in + used, sl, out, out_cap);
        if (d < 0) return -1;
        if (consumed) *consumed = used + sl;
        return 0;
    }
    if (sl >= out_cap) return -1;
    memcpy(out, in + used, sl);
    out[sl] = '\0';
    if (consumed) *consumed = used + sl;
    return 0;
}

int cmq_hpack_static_get(unsigned idx, const char **name,
                         const char **value) {
    if (idx == 0 || idx > CMQ_HPACK_STATIC_MAX) return -1;
    if (name) *name = cmq_hpack_static[idx].name;
    if (value) *value = cmq_hpack_static[idx].value;
    return 0;
}

int cmq_hpack_hdr_encode_indexed(unsigned idx, uint8_t *out, size_t cap) {
    if (idx == 0 || idx > CMQ_HPACK_STATIC_MAX) return -1;
    return cmq_hpack_int_encode((uint32_t)idx, 7, 0x80, out, cap);
}

int cmq_hpack_hdr_decode(const uint8_t *in, size_t in_len, char *name,
                         size_t ncap, char *value, size_t vcap,
                         size_t *consumed) {
    if (!in || !name || !value || in_len == 0 || ncap == 0 || vcap == 0)
        return -1;
    if ((in[0] & 0x80) == 0) return -1;
    uint32_t idx = 0;
    size_t used = 0;
    if (cmq_hpack_int_decode(in, in_len, 7, &idx, &used) != 0) return -1;
    const char *n = NULL, *v = NULL;
    if (cmq_hpack_static_get((unsigned)idx, &n, &v) != 0) return -1;
    if (strlen(n) >= ncap || strlen(v) >= vcap) return -1;
    memcpy(name, n, strlen(n) + 1);
    memcpy(value, v, strlen(v) + 1);
    if (consumed) *consumed = used;
    return 0;
}

void cmq_hpack_dyn_init(cmq_hpack_dyn_t *t) {
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->max = CMQ_HPACK_DYN_MAX;
}

unsigned cmq_hpack_dyn_size(const cmq_hpack_dyn_t *t) {
    return t ? t->size : 0;
}

unsigned cmq_hpack_dyn_count(const cmq_hpack_dyn_t *t) {
    return t ? t->n : 0;
}

static void cmq_hpack_dyn_evict_oldest(cmq_hpack_dyn_t *t) {
    if (!t || t->n == 0) return;
    unsigned last = t->n - 1;
    t->size -= (unsigned)t->e[last].nlen + (unsigned)t->e[last].vlen + 32u;
    t->n--;
    uint8_t tmp[CMQ_HPACK_DYN_MAX];
    unsigned u = 0;
    for (unsigned i = 0; i < t->n; i++) {
        memcpy(tmp + u, t->buf + t->e[i].noff, t->e[i].nlen);
        t->e[i].noff = (uint16_t)u;
        u += t->e[i].nlen;
        memcpy(tmp + u, t->buf + t->e[i].voff, t->e[i].vlen);
        t->e[i].voff = (uint16_t)u;
        u += t->e[i].vlen;
    }
    if (u > 0) memcpy(t->buf, tmp, u);
    t->used = u;
}

int cmq_hpack_dyn_set_max(cmq_hpack_dyn_t *t, unsigned max) {
    if (!t || max > CMQ_HPACK_DYN_MAX) return -1;
    t->max = max;
    while (t->n > 0 && t->size > t->max)
        cmq_hpack_dyn_evict_oldest(t);
    return 0;
}

int cmq_hpack_dyn_add(cmq_hpack_dyn_t *t, const char *name, const char *value) {
    if (!t || !name || !value) return -1;
    size_t nl = strlen(name);
    size_t vl = strlen(value);
    if (nl == 0 || vl == 0 || nl > CMQ_HPACK_STR_MAX || vl > CMQ_HPACK_STR_MAX)
        return -1;
    unsigned es = (unsigned)nl + (unsigned)vl + 32u;
    while (t->n > 0 && (t->size + es > t->max || t->n >= CMQ_HPACK_DYN_SLOTS))
        cmq_hpack_dyn_evict_oldest(t);
    if (es > t->max) return 0;
    if (t->used + nl + vl > CMQ_HPACK_DYN_MAX) return -1;
    if (t->n > 0)
        memmove(&t->e[1], &t->e[0], t->n * sizeof(t->e[0]));
    t->e[0].noff = (uint16_t)t->used;
    t->e[0].nlen = (uint16_t)nl;
    memcpy(t->buf + t->used, name, nl);
    t->used += (unsigned)nl;
    t->e[0].voff = (uint16_t)t->used;
    t->e[0].vlen = (uint16_t)vl;
    memcpy(t->buf + t->used, value, vl);
    t->used += (unsigned)vl;
    t->n++;
    t->size += es;
    return 0;
}

int cmq_hpack_dyn_get(const cmq_hpack_dyn_t *t, unsigned newest_off,
                      const char **name, size_t *nlen, const char **value,
                      size_t *vlen) {
    if (!t || !name || !nlen || !value || !vlen || newest_off >= t->n)
        return -1;
    *name = (const char *)(t->buf + t->e[newest_off].noff);
    *nlen = t->e[newest_off].nlen;
    *value = (const char *)(t->buf + t->e[newest_off].voff);
    *vlen = t->e[newest_off].vlen;
    return 0;
}

static int cmq_hpack_copy_nv(const char *n, size_t nl, const char *v, size_t vl,
                             char *name, size_t ncap, char *value, size_t vcap) {
    if (nl >= ncap || vl >= vcap) return -1;
    memcpy(name, n, nl);
    name[nl] = '\0';
    memcpy(value, v, vl);
    value[vl] = '\0';
    return 0;
}

static int cmq_hpack_lookup(const cmq_hpack_dyn_t *t, unsigned idx, char *name,
                            size_t ncap, char *value, size_t vcap) {
    if (idx == 0) return -1;
    if (idx <= CMQ_HPACK_STATIC_MAX) {
        const char *n = NULL, *v = NULL;
        if (cmq_hpack_static_get(idx, &n, &v) != 0) return -1;
        return cmq_hpack_copy_nv(n, strlen(n), v, strlen(v), name, ncap, value,
                                 vcap);
    }
    const char *n = NULL, *v = NULL;
    size_t nl = 0, vl = 0;
    unsigned off = idx - (CMQ_HPACK_STATIC_MAX + 1u);
    if (cmq_hpack_dyn_get(t, off, &n, &nl, &v, &vl) != 0) return -1;
    return cmq_hpack_copy_nv(n, nl, v, vl, name, ncap, value, vcap);
}

int cmq_hpack_hdr_encode_inc(cmq_hpack_dyn_t *t, const char *name,
                             const char *value, uint8_t *out, size_t cap) {
    if (!t || !name || !value || !out || cap == 0) return -1;
    int n = cmq_hpack_int_encode(0, 6, 0x40, out, cap);
    if (n < 0) return -1;
    int n2 = cmq_hpack_str_encode(name, out + n, cap - (size_t)n);
    if (n2 < 0) return -1;
    int n3 = cmq_hpack_str_encode(value, out + n + n2,
                                  cap - (size_t)n - (size_t)n2);
    if (n3 < 0) return -1;
    if (cmq_hpack_dyn_add(t, name, value) != 0) return -1;
    return n + n2 + n3;
}

int cmq_hpack_hdr_decode_dyn(cmq_hpack_dyn_t *t, const uint8_t *in,
                             size_t in_len, char *name, size_t ncap,
                             char *value, size_t vcap, size_t *consumed) {
    if (!t || !in || !name || !value || in_len == 0 || ncap == 0 || vcap == 0)
        return -1;
    uint8_t b = in[0];
    size_t used = 0;
    if ((b & 0x80) != 0) {
        uint32_t idx = 0;
        if (cmq_hpack_int_decode(in, in_len, 7, &idx, &used) != 0) return -1;
        if (cmq_hpack_lookup(t, (unsigned)idx, name, ncap, value, vcap) != 0)
            return -1;
        if (consumed) *consumed = used;
        return 0;
    }
    if ((b & 0xe0) == 0x20) {
        uint32_t sz = 0;
        if (cmq_hpack_int_decode(in, in_len, 5, &sz, &used) != 0) return -1;
        if (cmq_hpack_dyn_set_max(t, sz) != 0) return -1;
        if (consumed) *consumed = used;
        return 1;
    }
    int incremental = 0;
    unsigned nbits = 0;
    if ((b & 0xc0) == 0x40) {
        incremental = 1;
        nbits = 6;
    } else if ((b & 0xf0) == 0x00 || (b & 0xf0) == 0x10) {
        nbits = 4;
    } else {
        return -1;
    }
    uint32_t nidx = 0;
    if (cmq_hpack_int_decode(in, in_len, nbits, &nidx, &used) != 0) return -1;
    if (used > in_len) return -1;
    if (nidx == 0) {
        size_t sl = 0;
        if (cmq_hpack_str_decode(in + used, in_len - used, name, ncap, &sl)
            != 0)
            return -1;
        used += sl;
    } else {
        char dummy[CMQ_HPACK_STR_MAX + 1];
        if (cmq_hpack_lookup(t, (unsigned)nidx, name, ncap, dummy,
                             sizeof(dummy)) != 0)
            return -1;
    }
    if (used > in_len) return -1;
    size_t sl = 0;
    if (cmq_hpack_str_decode(in + used, in_len - used, value, vcap, &sl) != 0)
        return -1;
    used += sl;
    if (incremental && cmq_hpack_dyn_add(t, name, value) != 0) return -1;
    if (consumed) *consumed = used;
    return 0;
}
