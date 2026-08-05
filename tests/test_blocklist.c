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

#define BLOCKLIST_TEST_FILE "/tmp/cmq-test-blocklist.txt"

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
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

TEST_MAIN()
