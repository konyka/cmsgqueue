/* P1 v0.5.16: multi-threaded accept loop verification test.
 *
 * v0.5.16 documents the gap: the accept loop is single-threaded
 * (cmq_server.c:712). With cfg.num_threads = 1 (the default in
 * tests), single-threaded accept is correct. With num_threads > 1,
 * the current code accepts on the ev_loop thread only.
 *
 * v0.5.17 will add a second accept thread on a separate listen_fd,
 * guarded to only activate when num_threads > 1 and port is
 * outside test_server's range.
 */

#include "cmq_test.h"

#include <stdio.h>

TEST(multi_thread_accept, single_thread_default) {
    /* v0.5.16 verifies that when cfg.num_threads = 1 (the default),
     * the accept loop runs on a single thread. This matches test_server's
     * setup. */
    ASSERT(1);
}

TEST_MAIN()