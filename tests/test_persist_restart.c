/* v0.5.37: end-to-end persistent subscriber recovery test.
 *
 * Subscriptions recorded via cmq_sublist_persist_record_sub must
 * survive a server destroy + recreate cycle. The v0.5.19 / v0.5.7
 * unit tests cover the persist file format in isolation; v0.5.37
 * tests the full server_create recovery path that runs at startup.
 */

#include "cmq_test.h"
#include "cmq_server.h"
#include "cmq_sublist.h"
#include "cmq_sublist_persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RESTART_DIR "/tmp/cmq-test-v0537-restart"

TEST(persist_restart, subscriber_survives_restart) {
    system("rm -rf " RESTART_DIR " && mkdir -p " RESTART_DIR);

    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 25070;
    cfg.persist_dir = RESTART_DIR;
    cfg.log_to_stdout = 0;

    /* Phase 1: server A. */
    cmq_server_t *srv_a = NULL;
    ASSERT_EQ(cmq_server_create(&srv_a, &cfg), CMQ_OK);
    ASSERT_NOT_NULL(srv_a->persist);
    ASSERT_EQ(cmq_sublist_persist_record_sub(srv_a->persist,
                                              1,
                                              "v0.5.37/sentinel",
                                              "user1"),
              0);
    cmq_sublist_persist_close(srv_a->persist);
    srv_a->persist = NULL;
    cmq_server_destroy(srv_a);

    /* Phase 2: server B (fresh process, same persist_dir). */
    cmq_server_t *srv_b = NULL;
    ASSERT_EQ(cmq_server_create(&srv_b, &cfg), CMQ_OK);
    size_t recovered = cmq_sublist_count(srv_b->sublist);
    ASSERT(recovered >= 1);

    cmq_sublist_result_t result;
    ASSERT_EQ(cmq_sublist_match(srv_b->sublist, "v0.5.37/sentinel",
                                 &result), 0);
    ASSERT_EQ(result.count, 1);

    cmq_server_destroy(srv_b);
    system("rm -rf " RESTART_DIR);
}

TEST_MAIN()
