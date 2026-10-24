/* F15: Connection blocklist.
 *
 * Reject banned IPs at the accept_cb path. The blocklist is a
 * file with one CIDR or IP per line, loaded at startup and
 * reloadable via cmq_blocklist_reload().
 *
 * Default: no blocklist (all IPs admitted).
 */

#include "cmq_test.h"
#include "cmq_blocklist.h"
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define BLOCKLIST_TEST_FILE "/tmp/cmq-test-blocklist.txt"

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

static struct sockaddr_in6 ipv6_address(const char *text) {
    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    (void)inet_pton(AF_INET6, text, &addr.sin6_addr);
    return addr;
}

TEST(blocklist, empty_admits_all) {
    write_file(BLOCKLIST_TEST_FILE, "");
    cmq_blocklist_t *bl = cmq_blocklist_load(BLOCKLIST_TEST_FILE);
    ASSERT_NOT_NULL(bl);
    ASSERT_EQ(cmq_blocklist_check(bl, htonl(0x0A000001)), 0);
    cmq_blocklist_free(bl);
}

TEST(blocklist, single_ip_blocked) {
    write_file(BLOCKLIST_TEST_FILE, "10.0.0.1\n");
    cmq_blocklist_t *bl = cmq_blocklist_load(BLOCKLIST_TEST_FILE);
    ASSERT_NOT_NULL(bl);
    ASSERT_EQ(cmq_blocklist_check(bl, htonl(0x0A000001)), 1);
    ASSERT_EQ(cmq_blocklist_check(bl, htonl(0x0A000002)), 0);
    cmq_blocklist_free(bl);
}

TEST(blocklist, multiple_ips) {
    write_file(BLOCKLIST_TEST_FILE,
        "10.0.0.1\n"
        "192.168.0.0/16\n"
        "172.16.5.5\n");
    cmq_blocklist_t *bl = cmq_blocklist_load(BLOCKLIST_TEST_FILE);
    ASSERT_NOT_NULL(bl);
    ASSERT_EQ(cmq_blocklist_check(bl, htonl(0x0A000001)), 1);
    ASSERT_EQ(cmq_blocklist_check(bl, htonl(0xC0A80001)), 1);  /* 192.168.0.0/16 */
    ASSERT_EQ(cmq_blocklist_check(bl, htonl(0xAC100505)), 1); /* 172.16.5.5 */
    ASSERT_EQ(cmq_blocklist_check(bl, htonl(0x08080808)), 0);  /* 8.8.8.8 */
    cmq_blocklist_free(bl);
}

TEST(blocklist, malformed_lines_skipped) {
    write_file(BLOCKLIST_TEST_FILE,
        "not-an-ip\n"
        "10.0.0.1\n"
        "999.999.999.999\n");
    cmq_blocklist_t *bl = cmq_blocklist_load(BLOCKLIST_TEST_FILE);
    ASSERT_NOT_NULL(bl);
    /* 10.0.0.1 still matches; malformed lines ignored. */
    ASSERT_EQ(cmq_blocklist_check(bl, htonl(0x0A000001)), 1);
    cmq_blocklist_free(bl);
}

TEST(blocklist, ipv6_exact_and_prefixes) {
    write_file(BLOCKLIST_TEST_FILE,
        "2001:db8::1\n"
        "2001:db8:1234::/64\n");
    cmq_blocklist_t *bl = cmq_blocklist_load(BLOCKLIST_TEST_FILE);
    ASSERT_NOT_NULL(bl);

    struct sockaddr_in6 exact = ipv6_address("2001:db8::1");
    struct sockaddr_in6 other = ipv6_address("2001:db8::2");
    struct sockaddr_in6 subnet = ipv6_address("2001:db8:1234::abcd");
    ASSERT_EQ(cmq_blocklist_check_sockaddr(bl,
                                           (const struct sockaddr *)&exact,
                                           sizeof(exact)), 1);
    ASSERT_EQ(cmq_blocklist_check_sockaddr(bl,
                                           (const struct sockaddr *)&other,
                                           sizeof(other)), 0);
    ASSERT_EQ(cmq_blocklist_check_sockaddr(bl,
                                           (const struct sockaddr *)&subnet,
                                           sizeof(subnet)), 1);
    cmq_blocklist_free(bl);
}

TEST(blocklist, ipv6_zero_prefix) {
    write_file(BLOCKLIST_TEST_FILE, "::/0\n");
    cmq_blocklist_t *bl = cmq_blocklist_load(BLOCKLIST_TEST_FILE);
    ASSERT_NOT_NULL(bl);

    struct sockaddr_in6 address = ipv6_address("2001:db8:ffff::42");
    ASSERT_EQ(cmq_blocklist_check_sockaddr(bl,
                                           (const struct sockaddr *)&address,
                                           sizeof(address)), 1);
    cmq_blocklist_free(bl);
}

TEST(blocklist, ipv6_non_match_and_mixed_families) {
    write_file(BLOCKLIST_TEST_FILE,
        "10.0.0.0/8\n"
        "2001:db8:1::/64\n");
    cmq_blocklist_t *bl = cmq_blocklist_load(BLOCKLIST_TEST_FILE);
    ASSERT_NOT_NULL(bl);

    struct sockaddr_in6 blocked = ipv6_address("2001:db8:1::5");
    struct sockaddr_in6 admitted = ipv6_address("2001:db8:2::5");
    ASSERT_EQ(cmq_blocklist_check_sockaddr(bl,
                                           (const struct sockaddr *)&blocked,
                                           sizeof(blocked)), 1);
    ASSERT_EQ(cmq_blocklist_check_sockaddr(bl,
                                           (const struct sockaddr *)&admitted,
                                           sizeof(admitted)), 0);
    ASSERT_EQ(cmq_blocklist_check(bl, htonl(0x0A010203)), 1);
    ASSERT_EQ(cmq_blocklist_check(bl, htonl(0xC0000201)), 0);
    cmq_blocklist_free(bl);
}

TEST(blocklist, ipv6_malformed_prefixes_skipped) {
    write_file(BLOCKLIST_TEST_FILE,
        "2001:db8::1/129\n"
        "2001:db8::2/-1\n"
        "2001:db8::3/not-a-prefix\n"
        "2001:db8::4/64extra\n"
        "2001:db8::5/128\n");
    cmq_blocklist_t *bl = cmq_blocklist_load(BLOCKLIST_TEST_FILE);
    ASSERT_NOT_NULL(bl);

    struct sockaddr_in6 invalid = ipv6_address("2001:db8::1");
    struct sockaddr_in6 valid = ipv6_address("2001:db8::5");
    ASSERT_EQ(cmq_blocklist_check_sockaddr(bl,
                                           (const struct sockaddr *)&invalid,
                                           sizeof(invalid)), 0);
    ASSERT_EQ(cmq_blocklist_check_sockaddr(bl,
                                           (const struct sockaddr *)&valid,
                                           sizeof(valid)), 1);
    cmq_blocklist_free(bl);
}

TEST_MAIN()
