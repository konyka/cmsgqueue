/* F17: route TLS session API tests. */

#include "cmq_test.h"
#include "cmq_route_tls_sess.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>

TEST(route_tls_sess, create_with_null_ctx_returns_null) {
    cmq_route_tls_sess_t *sess = cmq_route_tls_sess_create(0, NULL);
    ASSERT(sess == NULL);
}

TEST(route_tls_sess, read_with_null_sess_returns_error) {
    ssize_t n = cmq_route_tls_sess_read(NULL, -1, NULL, 0);
    ASSERT_EQ(n, -1);
}

TEST(route_tls_sess, write_with_null_sess_returns_error) {
    ssize_t n = cmq_route_tls_sess_write(NULL, -1, NULL, 0);
    ASSERT_EQ(n, -1);
}

TEST(route_tls_sess, destroy_null_safe) {
    cmq_route_tls_sess_destroy(NULL);
}

TEST_MAIN()
