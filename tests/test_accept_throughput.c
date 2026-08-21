/* P2 v0.5.11: accept throughput test (verification).
 *
 * v0.5.11 documents the gap: the accept loop is single-threaded
 * (cmq_server.c:712). The throughput test (1 acceptor vs 2 acceptors)
 * is deferred to v0.6 when the multi-threaded accept work ships.
 */

#include "cmq_test.h"

#include <stdio.h>

TEST(accept_throughput, single_acceptor_baseline) {
    /* P1 v0.5.11 deferred. The benchmark baseline (~33K msg/s) is
     * measured with a single accept thread. v0.6 ships the
     * multi-threaded accept loop; this test will then exercise
     * 2 acceptors and assert 2x throughput. */
    ASSERT(1);
}

TEST_MAIN()