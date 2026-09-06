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
    if (in[0] & 0x80) return -1; /* Huffman deferred */
    uint32_t sl = 0;
    size_t used = 0;
    if (cmq_hpack_int_decode(in, in_len, 7, &sl, &used) != 0) return -1;
    if (sl == 0 || sl > CMQ_HPACK_STR_MAX || sl >= out_cap) return -1;
    if (used + sl > in_len) return -1;
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
