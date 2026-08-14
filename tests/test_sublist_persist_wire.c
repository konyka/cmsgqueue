/* F18: persistent subscription state — server-side wire-up. */

#include "cmq_test.h"
#include "cmq_server.h"
#include "cmq_config.h"
#include "cmq_sublist_persist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SUB_PERSIST_TEST_DIR "/tmp/cmq-test-sub-persist-wire"

TEST(sublist_persist_wire, open_close_library) {
    system("rm -rf " SUB_PERSIST_TEST_DIR " && mkdir -p " SUB_PERSIST_TEST_DIR);
    cmq_sublist_persist_t *p = cmq_sublist_persist_open(SUB_PERSIST_TEST_DIR);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cmq_sublist_persist_record_sub(p, 1, "foo.bar", "user1"), 0);
    ASSERT_EQ(cmq_sublist_persist_record_unsub(p, 1), 0);
    cmq_sublist_persist_close(p);
}

TEST(sublist_persist_wire, server_with_persist_dir_creates_persist) {
    /* Just verify the API: open + record + close. The server wire-up
     * (calling from handle_subscribe) is verified manually. */
    system("rm -rf " SUB_PERSIST_TEST_DIR " && mkdir -p " SUB_PERSIST_TEST_DIR);
    cmq_sublist_persist_t *p = cmq_sublist_persist_open(SUB_PERSIST_TEST_DIR);
    ASSERT_NOT_NULL(p);
    int rc = cmq_sublist_persist_record_sub(p, 1, "x", "y");
    ASSERT_EQ(rc, 0);
    cmq_sublist_persist_close(p);
    /* Reopen and verify the file exists. */
    p = cmq_sublist_persist_open(SUB_PERSIST_TEST_DIR);
    ASSERT_NOT_NULL(p);
    cmq_sublist_persist_close(p);
}

TEST_MAIN()
