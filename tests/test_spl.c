/* v0.5.150: reload loads persisted subs when persist was just attached. */
#include "cmq_sublist_persist.h"
#include "cmq_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SPL_DIR "build-tdd/spl"

static int g_subs;
static char g_subj[64];

static int spl_cb(void *ctx, int is_sub, uint64_t sub_id,
                  const char *subject, const char *account) {
    (void)ctx;
    (void)sub_id;
    (void)account;
    if (is_sub) {
        g_subs++;
        snprintf(g_subj, sizeof(g_subj), "%s", subject ? subject : "");
    }
    return 0;
}

static int spl_prepare(void) {
    (void)system("rm -rf " SPL_DIR " && mkdir -p " SPL_DIR);
    cmq_sublist_persist_t *p = cmq_sublist_persist_open(SPL_DIR);
    if (!p) return -1;
    int rc = cmq_sublist_persist_record_sub(p, 1, "foo.bar", "acc");
    cmq_sublist_persist_close(p);
    return rc;
}

TEST(spl, apply) {
    ASSERT_EQ(spl_prepare(), 0);
    cmq_sublist_persist_t *p = cmq_sublist_persist_open(SPL_DIR);
    ASSERT_NOT_NULL(p);
    int loaded = 0;
    g_subs = 0;
    g_subj[0] = '\0';
    ASSERT_EQ(cmq_sublist_persist_reload_load(p, &loaded, spl_cb, NULL), 0);
    ASSERT_EQ(loaded, 1);
    ASSERT_EQ(g_subs, 1);
    ASSERT_STR_EQ(g_subj, "foo.bar");
    int keep = loaded;
    g_subs = 0;
    ASSERT_EQ(cmq_sublist_persist_reload_load(p, &loaded, spl_cb, NULL), 0);
    ASSERT_EQ(loaded, keep);
    ASSERT_EQ(g_subs, 0);
    cmq_sublist_persist_close(p);
}

TEST(spl, omitted) {
    int loaded = 0;
    ASSERT_EQ(cmq_sublist_persist_reload_load(NULL, &loaded, spl_cb, NULL), 0);
    ASSERT_EQ(loaded, 0);
}

TEST(spl, empty) {
    (void)system("rm -rf " SPL_DIR " && mkdir -p " SPL_DIR);
    cmq_sublist_persist_t *p = cmq_sublist_persist_open(SPL_DIR);
    ASSERT_NOT_NULL(p);
    int loaded = 0;
    g_subs = 0;
    ASSERT_EQ(cmq_sublist_persist_reload_load(p, &loaded, spl_cb, NULL), 0);
    ASSERT_EQ(loaded, 1);
    ASSERT_EQ(g_subs, 0);
    cmq_sublist_persist_close(p);
}

TEST(spl, reject) {
    ASSERT(cmq_sublist_persist_reload_load((cmq_sublist_persist_t *)1,
                                           NULL, spl_cb, NULL) != 0);
    (void)system("rm -rf " SPL_DIR " && mkdir -p " SPL_DIR);
    cmq_sublist_persist_t *p = cmq_sublist_persist_open(SPL_DIR);
    ASSERT_NOT_NULL(p);
    int loaded = 0;
    ASSERT(cmq_sublist_persist_reload_load(p, &loaded, NULL, NULL) != 0);
    ASSERT_EQ(loaded, 0);
    cmq_sublist_persist_close(p);
}

TEST_MAIN()
