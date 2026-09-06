/* v0.5.47: connz / subz / routez JSON formatters. */
#include "cmq_test.h"
#include "cmq_monitor.h"
#include <string.h>

TEST(monitor, json_escape) {
    char out[64];
    ASSERT_EQ(cmq_json_escape("a\"b\\c", out, sizeof(out)), 0);
    ASSERT_EQ(strcmp(out, "a\\\"b\\\\c"), 0);
    ASSERT_EQ(cmq_json_escape("x\ny", out, sizeof(out)), 0);
    ASSERT_EQ(strcmp(out, "x\\ny"), 0);
}

TEST(monitor, connz_empty) {
    char buf[256];
    int n = cmq_monitor_format_connz(buf, sizeof(buf), NULL, 0);
    ASSERT(n > 0);
    ASSERT(strstr(buf, "\"num_connections\":0") != NULL);
    ASSERT(strstr(buf, "\"connections\":[]") != NULL);
}

TEST(monitor, connz_escapes_user) {
    cmq_monitor_conn_t c;
    memset(&c, 0, sizeof(c));
    c.cid = 7;
    c.state = 1;
    strcpy(c.account, "ac\"c");
    strcpy(c.user, "u\\n");
    c.nsubs = 2;
    strcpy(c.tid, "aabb");
    char buf[512];
    ASSERT(cmq_monitor_format_connz(buf, sizeof(buf), &c, 1) > 0);
    ASSERT(strstr(buf, "\"cid\":7") != NULL);
    ASSERT(strstr(buf, "\"state\":\"connected\"") != NULL);
    ASSERT(strstr(buf, "\"account\":\"ac\\\"c\"") != NULL);
    ASSERT(strstr(buf, "\"user\":\"u\\\\n\"") != NULL);
    ASSERT(strstr(buf, "\"tid\":\"aabb\"") != NULL);
}

TEST(monitor, subz_one) {
    cmq_monitor_sub_t s;
    memset(&s, 0, sizeof(s));
    s.cid = 1;
    s.sid = 3;
    strcpy(s.subject, "foo.bar");
    strcpy(s.queue, "q1");
    char buf[256];
    ASSERT(cmq_monitor_format_subz(buf, sizeof(buf), &s, 1) > 0);
    ASSERT(strstr(buf, "\"num_subscriptions\":1") != NULL);
    ASSERT(strstr(buf, "\"subject\":\"foo.bar\"") != NULL);
    ASSERT(strstr(buf, "\"queue\":\"q1\"") != NULL);
}

TEST(monitor, routez_one) {
    cmq_monitor_route_t r;
    memset(&r, 0, sizeof(r));
    strcpy(r.id, "n1");
    strcpy(r.addr, "10.0.0.1");
    r.port = 7654;
    r.connected = 1;
    r.fd = 5;
    char buf[256];
    ASSERT(cmq_monitor_format_routez(buf, sizeof(buf), 1, 1, 1, &r, 1) > 0);
    ASSERT(strstr(buf, "\"live\":1") != NULL);
    ASSERT(strstr(buf, "\"id\":\"n1\"") != NULL);
    ASSERT(strstr(buf, "\"port\":7654") != NULL);
}

TEST_MAIN()
