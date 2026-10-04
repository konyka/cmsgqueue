/* v0.5.69: D2 HTTP/2 preface, SETTINGS, 32-stream table. */
#include "cmq_test.h"
#include "cmq_h2.h"
#include <string.h>

static void put_hdr(uint8_t *b, uint32_t len, uint8_t type, uint8_t flags,
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

TEST(h2, preface) {
    ASSERT_EQ(cmq_h2_preface_ok(cmq_h2_preface, CMQ_H2_PREFACE_LEN), 0);
    uint8_t bad[CMQ_H2_PREFACE_LEN];
    memcpy(bad, cmq_h2_preface, CMQ_H2_PREFACE_LEN);
    bad[0] ^= 1;
    ASSERT(cmq_h2_preface_ok(bad, CMQ_H2_PREFACE_LEN) != 0);
    ASSERT(cmq_h2_preface_ok(cmq_h2_preface, 8) != 0);
}

TEST(h2, settings) {
    uint8_t buf[16];
    int n = cmq_h2_settings_encode(CMQ_H2_MAX_STREAMS, buf, sizeof(buf));
    ASSERT_EQ(n, 6);
    uint32_t ms = 0;
    ASSERT_EQ(cmq_h2_settings_decode(buf, (size_t)n, &ms), 0);
    ASSERT_EQ(ms, (uint32_t)CMQ_H2_MAX_STREAMS);
    uint32_t len = 0, sid = 99;
    uint8_t type = 0, flags = 0;
    uint8_t frame[32];
    put_hdr(frame, 6, CMQ_H2_TYPE_SETTINGS, 0, 0);
    memcpy(frame + 9, buf, 6);
    ASSERT_EQ(cmq_h2_frame_hdr(frame, 15, &len, &type, &flags, &sid), 0);
    ASSERT_EQ(len, (uint32_t)6);
    ASSERT_EQ(type, (uint8_t)CMQ_H2_TYPE_SETTINGS);
    ASSERT_EQ(sid, (uint32_t)0);
}

TEST(h2, streams_cap) {
    cmq_h2_t *h = cmq_h2_create();
    ASSERT_NOT_NULL(h);
    size_t used = 0;
    ASSERT_EQ(cmq_h2_feed(h, cmq_h2_preface, CMQ_H2_PREFACE_LEN, &used), 1);
    ASSERT_EQ(cmq_h2_state(h), CMQ_H2_ST_SETTINGS);
    uint8_t set[15];
    uint8_t payload[6];
    ASSERT_EQ(cmq_h2_settings_encode(32, payload, sizeof(payload)), 6);
    put_hdr(set, 6, CMQ_H2_TYPE_SETTINGS, 0, 0);
    memcpy(set + 9, payload, 6);
    ASSERT_EQ(cmq_h2_feed(h, set, 15, &used), 1);
    ASSERT_EQ(cmq_h2_state(h), CMQ_H2_ST_OPEN);
    uint8_t hdr[9];
    for (uint32_t i = 0; i < 32; i++) {
        put_hdr(hdr, 0, CMQ_H2_TYPE_HEADERS, 0x04, i * 2 + 1);
        ASSERT_EQ(cmq_h2_feed(h, hdr, 9, &used), 1);
    }
    ASSERT_EQ(cmq_h2_stream_count(h), 32);
    put_hdr(hdr, 0, CMQ_H2_TYPE_HEADERS, 0x04, 65);
    ASSERT(cmq_h2_feed(h, hdr, 9, &used) < 0);
    ASSERT_EQ(cmq_h2_state(h), CMQ_H2_ST_GOAWAY);
    cmq_h2_destroy(h);
}

TEST(h2, reject) {
    cmq_h2_t *h = cmq_h2_create();
    size_t used = 0;
    uint8_t bad[CMQ_H2_PREFACE_LEN];
    memset(bad, 0, sizeof(bad));
    ASSERT(cmq_h2_feed(h, bad, sizeof(bad), &used) < 0);
    ASSERT_EQ(cmq_h2_state(h), CMQ_H2_ST_GOAWAY);
    cmq_h2_destroy(h);
    h = cmq_h2_create();
    ASSERT_EQ(cmq_h2_feed(h, cmq_h2_preface, CMQ_H2_PREFACE_LEN, &used), 1);
    uint8_t ev[9];
    put_hdr(ev, 0, CMQ_H2_TYPE_HEADERS, 0x04, 2);
    ASSERT(cmq_h2_feed(h, ev, 9, &used) < 0);
    cmq_h2_destroy(h);
    ASSERT(cmq_h2_frame_hdr(NULL, 0, NULL, NULL, NULL, NULL) != 0);
}

TEST_MAIN()
