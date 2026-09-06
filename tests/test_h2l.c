/* v0.5.73: D2 dedicated HTTP/2 listener (prior-knowledge POST). */
#include "cmq_test.h"
#include "cmq_h2.h"
#include "cmq_hpack.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

static int write_all(int fd, const uint8_t *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, p + off, n - off, 0);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

static int client_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int build_post(uint8_t *out, size_t cap, const char *path,
                      const uint8_t *body, size_t blen) {
    uint8_t blk[128];
    size_t bo = 0;
    blk[bo++] = 0x83; /* :method POST */
    int n = cmq_hpack_int_encode(4, 6, 0x40, blk + bo, sizeof(blk) - bo);
    if (n < 0) return -1;
    bo += (size_t)n;
    n = cmq_hpack_str_encode(path, blk + bo, sizeof(blk) - bo);
    if (n < 0) return -1;
    bo += (size_t)n;
    if (9 + bo + 9 + blen > cap) return -1;
    size_t o = 0;
    memcpy(out + o, cmq_h2_preface, CMQ_H2_PREFACE_LEN);
    o += CMQ_H2_PREFACE_LEN;
    uint8_t setpl[6];
    ASSERT_EQ(cmq_h2_settings_encode(32, setpl, sizeof(setpl)), 6);
    put_hdr(out + o, 6, CMQ_H2_TYPE_SETTINGS, 0, 0);
    memcpy(out + o + 9, setpl, 6);
    o += 15;
    put_hdr(out + o, (uint32_t)bo, CMQ_H2_TYPE_HEADERS, 0x04, 1);
    memcpy(out + o + 9, blk, bo);
    o += 9 + bo;
    put_hdr(out + o, (uint32_t)blen, CMQ_H2_TYPE_DATA, 0x01, 1);
    memcpy(out + o + 9, body, blen);
    o += 9 + blen;
    return (int)o;
}

TEST(h2l, listen_post) {
    int lfd = cmq_h2_listen(NULL, 0);
    ASSERT(lfd >= 0);
    int port = cmq_h2_listen_port(lfd);
    ASSERT(port > 0);
    int cfd = client_connect(port);
    ASSERT(cfd >= 0);
    uint8_t req[256];
    int n = build_post(req, sizeof(req), "/foo.bar", (const uint8_t *)"hi", 2);
    ASSERT(n > 0);
    ASSERT_EQ(write_all(cfd, req, (size_t)n), 0);
    char sub[32];
    uint8_t body[16];
    size_t blen = 0;
    ASSERT_EQ(cmq_h2_accept(lfd, sub, sizeof(sub), body, sizeof(body), &blen),
              0);
    ASSERT_STR_EQ(sub, "foo.bar");
    ASSERT_EQ(blen, 2u);
    ASSERT(memcmp(body, "hi", 2) == 0);
    close(cfd);
    close(lfd);
}

TEST(h2l, bad_preface) {
    int lfd = cmq_h2_listen("127.0.0.1", 0);
    ASSERT(lfd >= 0);
    int cfd = client_connect(cmq_h2_listen_port(lfd));
    ASSERT(cfd >= 0);
    uint8_t junk[24];
    memset(junk, 0x41, sizeof(junk));
    ASSERT_EQ(write_all(cfd, junk, sizeof(junk)), 0);
    char sub[8];
    uint8_t body[8];
    size_t blen = 0;
    ASSERT(cmq_h2_accept(lfd, sub, sizeof(sub), body, sizeof(body), &blen) < 0);
    close(cfd);
    close(lfd);
}

TEST(h2l, bad_path) {
    int lfd = cmq_h2_listen(NULL, 0);
    ASSERT(lfd >= 0);
    int cfd = client_connect(cmq_h2_listen_port(lfd));
    ASSERT(cfd >= 0);
    uint8_t req[256];
    int n = build_post(req, sizeof(req), "/../x", (const uint8_t *)"z", 1);
    ASSERT(n > 0);
    ASSERT_EQ(write_all(cfd, req, (size_t)n), 0);
    char sub[16];
    uint8_t body[8];
    size_t blen = 0;
    ASSERT(cmq_h2_accept(lfd, sub, sizeof(sub), body, sizeof(body), &blen) < 0);
    close(cfd);
    close(lfd);
}

TEST(h2l, reject) {
    ASSERT(cmq_h2_listen("0.0.0.0", 0) < 0);
    ASSERT(cmq_h2_listen(NULL, -1) < 0);
    ASSERT(cmq_h2_listen_port(-1) < 0);
    char sub[8];
    uint8_t body[8];
    size_t blen = 0;
    ASSERT(cmq_h2_session(-1, sub, sizeof(sub), body, sizeof(body), &blen) < 0);
    ASSERT(cmq_h2_accept(-1, sub, sizeof(sub), body, sizeof(body), &blen) < 0);
}

TEST_MAIN()
