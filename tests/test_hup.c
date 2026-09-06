/* v0.5.140: reload binds h2_port when create had none. */
#include "cmq_h2.h"
#include "cmq_test.h"
#include <unistd.h>

TEST(hup, apply) {
    int probe = cmq_h2_listen("127.0.0.1", 0);
    ASSERT(probe >= 0);
    int port = cmq_h2_listen_port(probe);
    ASSERT(port > 0);
    close(probe);
    int lfd = -1;
    int live = 0;
    ASSERT_EQ(cmq_h2_reload_listen(&lfd, &live, port), 0);
    ASSERT(lfd >= 0);
    ASSERT_EQ(live, port);
    ASSERT_EQ(cmq_h2_listen_port(lfd), port);
    int same = lfd;
    ASSERT_EQ(cmq_h2_reload_listen(&lfd, &live, port + 1), 0);
    ASSERT(lfd == same);
    ASSERT_EQ(live, port);
    close(lfd);
}

TEST(hup, omitted) {
    int lfd = -1;
    int live = 0;
    ASSERT_EQ(cmq_h2_reload_listen(&lfd, &live, 0), 0);
    ASSERT_EQ(lfd, -1);
    ASSERT_EQ(live, 0);
}

TEST(hup, empty) {
    int lfd = -1;
    int live = 7;
    ASSERT_EQ(cmq_h2_reload_listen(&lfd, &live, 0), 0);
    ASSERT_EQ(lfd, -1);
    ASSERT_EQ(live, 7);
}

TEST(hup, reject) {
    int lfd = -1;
    int live = 9;
    ASSERT(cmq_h2_reload_listen(&lfd, &live, -1) != 0);
    ASSERT_EQ(lfd, -1);
    ASSERT_EQ(live, 9);
    ASSERT(cmq_h2_reload_listen(&lfd, &live, 65536) != 0);
    ASSERT_EQ(lfd, -1);
    ASSERT_EQ(live, 9);
    ASSERT(cmq_h2_reload_listen(NULL, &live, 1) != 0);
    ASSERT(cmq_h2_reload_listen(&lfd, NULL, 1) != 0);
}

TEST_MAIN()
