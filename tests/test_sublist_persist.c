/* F18: Persistent subscription state. */

#include "cmq_test.h"
#include "cmq_sublist_persist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SUB_PERSIST_DIR "/tmp/cmq-test-sub-persist"

static int g_subs_seen = 0;
static int g_unsubs_seen = 0;
static char g_last_subject[256] = {0};
static char g_last_account[256] = {0};

static int persist_cb(void *ctx, int is_sub, uint64_t sub_id,
                       const char *subject, const char *account) {
    (void)ctx;
    (void)sub_id;
    if (is_sub) {
        g_subs_seen++;
        snprintf(g_last_subject, sizeof(g_last_subject), "%s",
                 subject ? subject : "");
        snprintf(g_last_account, sizeof(g_last_account), "%s",
                 account ? account : "");
    } else {
        g_unsubs_seen++;
    }
    return 0;
}

TEST(sublist_persist, open_returns_non_null) {
    system("rm -rf " SUB_PERSIST_DIR " && mkdir -p " SUB_PERSIST_DIR);
    cmq_sublist_persist_t *p = cmq_sublist_persist_open(SUB_PERSIST_DIR);
    ASSERT_NOT_NULL(p);
    cmq_sublist_persist_close(p);
}

TEST(sublist_persist, record_then_load) {
    system("rm -rf " SUB_PERSIST_DIR " && mkdir -p " SUB_PERSIST_DIR);
    cmq_sublist_persist_t *p = cmq_sublist_persist_open(SUB_PERSIST_DIR);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cmq_sublist_persist_record_sub(p, 1, "foo.bar", "user1"), 0);
    ASSERT_EQ(cmq_sublist_persist_record_sub(p, 2, "baz", "user2"), 0);
    ASSERT_EQ(cmq_sublist_persist_record_unsub(p, 1), 0);
    cmq_sublist_persist_close(p);

    /* Reopen and load. */
    p = cmq_sublist_persist_open(SUB_PERSIST_DIR);
    ASSERT_NOT_NULL(p);
    g_subs_seen = 0;
    g_unsubs_seen = 0;
    g_last_subject[0] = '\0';
    g_last_account[0] = '\0';
    int n = cmq_sublist_persist_load(p, persist_cb, NULL);
    ASSERT_EQ(n, 3);
    ASSERT_EQ(g_subs_seen, 2);
    ASSERT_EQ(g_unsubs_seen, 1);
    cmq_sublist_persist_close(p);
}

TEST(sublist_persist, load_empty_file) {
    system("rm -rf " SUB_PERSIST_DIR " && mkdir -p " SUB_PERSIST_DIR);
    cmq_sublist_persist_t *p = cmq_sublist_persist_open(SUB_PERSIST_DIR);
    ASSERT_NOT_NULL(p);
    cmq_sublist_persist_close(p);
    p = cmq_sublist_persist_open(SUB_PERSIST_DIR);
    ASSERT_NOT_NULL(p);
    int n = cmq_sublist_persist_load(p, persist_cb, NULL);
    ASSERT_EQ(n, 0);
    cmq_sublist_persist_close(p);
}

TEST_MAIN()
