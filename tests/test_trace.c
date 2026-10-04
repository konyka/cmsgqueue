/* F11: Connection tracing (correlation IDs).
 *
 * Each connection gets a 16-byte UUID. The ID is set at CONNECT
 * time and propagated to log entries for that connection. The
 * ID is hex-encoded in log output.
 */

#include "cmq_test.h"
#include "cmq_trace.h"
#include <string.h>
#include <stdlib.h>

TEST(trace, generate_id_is_unique) {
    uint8_t id1[16], id2[16];
    cmq_trace_id(id1);
    cmq_trace_id(id2);
    ASSERT(memcmp(id1, id2, 16) != 0);
}

TEST(trace, encode_hex) {
    uint8_t id[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
    };
    char hex[33];
    int n = cmq_trace_id_hex(id, hex, sizeof(hex));
    ASSERT_EQ(n, 32);
    ASSERT_EQ(strcmp(hex, "00112233445566778899aabbccddeeff"), 0);
}

TEST(trace, encode_short_buffer_truncates) {
    uint8_t id[16];
    cmq_trace_id(id);
    char hex[10];
    int n = cmq_trace_id_hex(id, hex, sizeof(hex));
    /* Short buffer: writes what fits, null-terminates. */
    ASSERT(n <= 9);
    ASSERT(strlen(hex) < 10);
}

TEST(trace, assign_fills_hex) {
    uint8_t id[16];
    char hex[33];
    memset(id, 0, sizeof(id));
    hex[0] = '\0';
    cmq_trace_assign(id, hex);
    int nz = 0;
    for (int i = 0; i < 16; i++)
        if (id[i] != 0) nz = 1;
    ASSERT(nz);
    ASSERT_EQ((int)strlen(hex), 32);
    char again[33];
    ASSERT_EQ(cmq_trace_id_hex(id, again, sizeof(again)), 32);
    ASSERT_EQ(strcmp(hex, again), 0);
}

TEST_MAIN()
