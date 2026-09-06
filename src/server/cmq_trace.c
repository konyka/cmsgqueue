#define _POSIX_C_SOURCE 200809L
#include "cmq_trace.h"
#include <openssl/rand.h>
#include <string.h>

void cmq_trace_id(uint8_t out[16]) {
    if (!out) return;
    if (RAND_bytes(out, 16) != 1) {
        /* Fallback: time-based unique-ish bytes. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t v = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
        for (int i = 0; i < 16; i++) {
            out[i] = (uint8_t)(v >> ((i % 8) * 8));
            v = (v * 1103515245ULL + 12345ULL) & 0x7FFFFFFFFFFFFFFFULL;
        }
    }
}

int cmq_trace_id_hex(const uint8_t id[16], char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    static const char hex[] = "0123456789abcdef";
    size_t need = 33; /* 32 hex + null */
    size_t copy = (out_len < need) ? out_len : need;
    size_t i;
    for (i = 0; i + 1 < copy; i++) {
        out[i] = hex[(id[i / 2] >> ((1 - (i % 2)) * 4)) & 0xF];
    }
    if (copy > 0) out[copy - 1] = '\0';
    return (int)(copy - 1);
}

void cmq_trace_assign(uint8_t id[16], char hex[33]) {
    if (!id) return;
    cmq_trace_id(id);
    if (hex)
        cmq_trace_id_hex(id, hex, 33);
}
