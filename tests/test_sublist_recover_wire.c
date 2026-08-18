/* P3: F18 persistent subscription recovery on restart.
 *
 * Phase 1: server starts with persist_dir, no subs file.
 * Phase 2: pre-write a SUB record via cmq_sublist_persist_record_sub.
 * Phase 3: destroy server.
 * Phase 4: new server with same persist_dir — load path should re-insert
 *   the sub into srv->sublist (test via cmq_sublist_match).
 */

#include "cmq_test.h"
#include "cmq_server.h"
#include "cmq_sublist.h"
#include "cmq_sublist_persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define P3_DIR "/tmp/cmq-test-p3-sublist-recover"

TEST(sublist_persist_wire, restart_restores_sub) {
    system("rm -rf " P3_DIR " && mkdir -p " P3_DIR);

    /* Phase 1+2: write a SUB record directly. */
    cmq_sublist_persist_t *p = cmq_sublist_persist_open(P3_DIR);
    ASSERT_NOT_NULL(p);
    uint64_t sub_id = 42;
    ASSERT_EQ(cmq_sublist_persist_record_sub(p, sub_id, "recover.foo",
                                               "$default"), 0);
    cmq_sublist_persist_close(p);

    /* Phase 3+4: build a server with the same persist_dir. The load
     * path in cmq_server_create must re-insert "recover.foo" into
     * srv->sublist via cmq_sublist_recover_cb. */
    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19994;
    cfg.log_to_stdout = 0;
    cfg.persist_dir = P3_DIR;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    /* Verify the subject is in srv->sublist (recover path ran). */
    cmq_sublist_result_t result = {0};
    int rc = cmq_sublist_match(srv->sublist, "recover.foo", &result);
    /* The match itself returns the count; we want >= 1 (ghost ref). */
    printf("  match count = %zu\n", result.count);
    ASSERT(result.count >= 1);
    cmq_sublist_result_free(&result);

    cmq_server_destroy(srv);
    system("rm -rf " P3_DIR);
}

TEST(sublist_persist_wire, unsubscribe_ghost_refs_persist) {
    /* Ghost refs survive unsub records (no live client → no addressable
     * sub_id to remove). The match still finds the ref. */
    (void)0;
    int rc __attribute__((unused)) = system("rm -rf " P3_DIR " && mkdir -p " P3_DIR);

    cmq_sublist_persist_t *p = cmq_sublist_persist_open(P3_DIR);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cmq_sublist_persist_record_sub(p, 7, "remove.foo",
                                               "$default"), 0);
    ASSERT_EQ(cmq_sublist_persist_record_unsub(p, 7), 0);
    cmq_sublist_persist_close(p);

    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19993;
    cfg.log_to_stdout = 0;
    cfg.persist_dir = P3_DIR;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_OK);

    cmq_sublist_result_t result = {0};
    cmq_sublist_match(srv->sublist, "remove.foo", &result);
    printf("  match count after unsub replay = %zu (ghost ref persists)\n",
           result.count);
    ASSERT(result.count >= 1);
    cmq_sublist_result_free(&result);

    cmq_server_destroy(srv);
    int rc2 __attribute__((unused)) = system("rm -rf " P3_DIR);
    (void)rc2;
}

TEST_MAIN()