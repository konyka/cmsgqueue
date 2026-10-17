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

static int write_wal_line(const char *line) {
    char path[256];
    snprintf(path, sizeof(path), "%s/cmq-subs.wal", P3_DIR);
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    int rc = fputs(line, fp) == EOF ? -1 : 0;
    fclose(fp);
    return rc;
}

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
    ASSERT_EQ(rc, 0);
    /* The match itself returns the count; we want >= 1 (ghost ref). */
    printf("  match count = %zu\n", result.count);
    ASSERT(result.count >= 1);
    cmq_sublist_result_free(&result);

    cmq_server_destroy(srv);
    system("rm -rf " P3_DIR);
}

TEST(sublist_persist_wire, unsubscribe_ghost_refs_persist) {
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
    printf("  match count after unsub replay = %zu\n",
           result.count);
    ASSERT_EQ(result.count, 0);
    ASSERT_EQ(cmq_sublist_count(srv->sublist), 0);
    cmq_sublist_result_free(&result);

    cmq_server_destroy(srv);
    int rc2 __attribute__((unused)) = system("rm -rf " P3_DIR);
    (void)rc2;
}

TEST(sublist_persist_wire, rejects_overlong_subject_without_ref) {
    system("rm -rf " P3_DIR " && mkdir -p " P3_DIR);
    char subject[320];
    memset(subject, 'a', sizeof(subject) - 1);
    subject[sizeof(subject) - 1] = '\0';
    char line[384];
    snprintf(line, sizeof(line), "S 9 %s $default\n", subject);
    ASSERT_EQ(write_wal_line(line), 0);

    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19992;
    cfg.log_to_stdout = 0;
    cfg.persist_dir = P3_DIR;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_ERR_INVALID_ARG);
    ASSERT(srv == NULL);
    system("rm -rf " P3_DIR);
}

TEST(sublist_persist_wire, rejects_malformed_numeric_id) {
    system("rm -rf " P3_DIR " && mkdir -p " P3_DIR);
    ASSERT_EQ(write_wal_line("U -1\n"), 0);

    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19991;
    cfg.log_to_stdout = 0;
    cfg.persist_dir = P3_DIR;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_ERR_INVALID_ARG);
    ASSERT(srv == NULL);
    system("rm -rf " P3_DIR);
}

TEST(sublist_persist_wire, rejects_truncated_and_unknown_records) {
    system("rm -rf " P3_DIR " && mkdir -p " P3_DIR);
    ASSERT_EQ(write_wal_line("X 1\n"), 0);

    cmq_config_t cfg = {0};
    cfg.num_threads = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 19990;
    cfg.log_to_stdout = 0;
    cfg.persist_dir = P3_DIR;
    cmq_server_t *srv = NULL;
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_ERR_INVALID_ARG);
    ASSERT(srv == NULL);

    char long_line[1100];
    memset(long_line, 'a', sizeof(long_line) - 2);
    long_line[0] = 'S';
    long_line[1] = ' ';
    long_line[sizeof(long_line) - 2] = '\n';
    long_line[sizeof(long_line) - 1] = '\0';
    ASSERT_EQ(write_wal_line(long_line), 0);
    ASSERT_EQ(cmq_server_create(&srv, &cfg), CMQ_ERR_INVALID_ARG);
    ASSERT(srv == NULL);
    system("rm -rf " P3_DIR);
}

TEST_MAIN()
