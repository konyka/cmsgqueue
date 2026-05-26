#include "cmq_sublist.h"
#include "cmq_test.h"
#include <string.h>

TEST(sublist, create_destroy) {
    cmq_sublist_t *sl = cmq_sublist_create();
    ASSERT_NOT_NULL(sl);
    cmq_sublist_destroy(sl);
}

TEST(sublist, insert_basic) {
    cmq_sublist_t *sl = cmq_sublist_create();
    int rc = cmq_sublist_insert(sl, "foo.bar", (void *)1);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(cmq_sublist_count(sl), (size_t)1);
    cmq_sublist_destroy(sl);
}

TEST(sublist, insert_duplicate) {
    cmq_sublist_t *sl = cmq_sublist_create();
    cmq_sublist_insert(sl, "foo.bar", (void *)1);
    int rc = cmq_sublist_insert(sl, "foo.bar", (void *)2);
    ASSERT(rc != 0);
    ASSERT_EQ(cmq_sublist_count(sl), (size_t)1);
    cmq_sublist_destroy(sl);
}

TEST(sublist, remove_basic) {
    cmq_sublist_t *sl = cmq_sublist_create();
    cmq_sublist_insert(sl, "foo.bar", (void *)1);
    int rc = cmq_sublist_remove(sl, "foo.bar");
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(cmq_sublist_count(sl), (size_t)0);
    cmq_sublist_destroy(sl);
}

TEST(sublist, remove_nonexistent) {
    cmq_sublist_t *sl = cmq_sublist_create();
    int rc = cmq_sublist_remove(sl, "nope");
    ASSERT(rc != 0);
    cmq_sublist_destroy(sl);
}

TEST(sublist, match_exact) {
    cmq_sublist_t *sl = cmq_sublist_create();
    cmq_sublist_insert(sl, "foo.bar", (void *)100);
    cmq_sublist_insert(sl, "foo.baz", (void *)200);
    cmq_sublist_insert(sl, "qux.quux", (void *)300);

    cmq_sublist_result_t result;
    cmq_sublist_match(sl, "foo.bar", &result);

    ASSERT_EQ(result.count, (size_t)1);
    ASSERT_EQ((intptr_t)result.entries[0], (intptr_t)100);

    cmq_sublist_result_free(&result);
    cmq_sublist_destroy(sl);
}

TEST(sublist, match_no_match) {
    cmq_sublist_t *sl = cmq_sublist_create();
    cmq_sublist_insert(sl, "foo.bar", (void *)1);

    cmq_sublist_result_t result;
    cmq_sublist_match(sl, "foo.baz", &result);
    ASSERT_EQ(result.count, (size_t)0);
    cmq_sublist_result_free(&result);
    cmq_sublist_destroy(sl);
}

TEST(sublist, match_pwc_star) {
    cmq_sublist_t *sl = cmq_sublist_create();
    cmq_sublist_insert(sl, "foo.*", (void *)1);
    cmq_sublist_insert(sl, "foo.bar", (void *)2);

    cmq_sublist_result_t result;
    cmq_sublist_match(sl, "foo.baz", &result);
    ASSERT_EQ(result.count, (size_t)1);
    ASSERT_EQ((intptr_t)result.entries[0], (intptr_t)1);

    cmq_sublist_result_free(&result);
    cmq_sublist_destroy(sl);
}

TEST(sublist, match_fwc_gt) {
    cmq_sublist_t *sl = cmq_sublist_create();
    cmq_sublist_insert(sl, "foo.>", (void *)1);

    cmq_sublist_result_t result;
    cmq_sublist_match(sl, "foo.bar", &result);
    ASSERT_EQ(result.count, (size_t)1);

    cmq_sublist_result_free(&result);
    memset(&result, 0, sizeof(result));

    cmq_sublist_match(sl, "foo.bar.baz.qux", &result);
    ASSERT_EQ(result.count, (size_t)1);
    ASSERT_EQ((intptr_t)result.entries[0], (intptr_t)1);

    cmq_sublist_result_free(&result);
    cmq_sublist_destroy(sl);
}

TEST(sublist, match_multiple_wildcards) {
    cmq_sublist_t *sl = cmq_sublist_create();
    cmq_sublist_insert(sl, "foo.bar", (void *)1);
    cmq_sublist_insert(sl, "foo.*", (void *)2);
    cmq_sublist_insert(sl, "*.bar", (void *)3);
    cmq_sublist_insert(sl, "foo.>", (void *)4);
    cmq_sublist_insert(sl, ">", (void *)5);

    cmq_sublist_result_t result;
    cmq_sublist_match(sl, "foo.bar", &result);
    ASSERT_EQ(result.count, (size_t)5);

    cmq_sublist_result_free(&result);
    cmq_sublist_destroy(sl);
}

TEST(sublist, invalid_subject_empty) {
    cmq_sublist_t *sl = cmq_sublist_create();
    int rc = cmq_sublist_insert(sl, "", (void *)1);
    ASSERT(rc != 0);
    cmq_sublist_destroy(sl);
}

TEST(sublist, invalid_subject_gt_middle) {
    cmq_sublist_t *sl = cmq_sublist_create();
    int rc = cmq_sublist_insert(sl, "foo.>.bar", (void *)1);
    ASSERT(rc != 0);
    cmq_sublist_destroy(sl);
}

TEST(sublist, many_subjects) {
    cmq_sublist_t *sl = cmq_sublist_create();
    for (int i = 0; i < 1000; i++) {
        char subject[64];
        snprintf(subject, sizeof(subject), "app.%d.event", i);
        cmq_sublist_insert(sl, subject, (void *)(intptr_t)(i + 1));
    }
    ASSERT_EQ(cmq_sublist_count(sl), (size_t)1000);

    cmq_sublist_result_t result;
    cmq_sublist_match(sl, "app.500.event", &result);
    ASSERT_EQ(result.count, (size_t)1);
    ASSERT_EQ((intptr_t)result.entries[0], (intptr_t)501);

    cmq_sublist_result_free(&result);
    cmq_sublist_destroy(sl);
}

TEST(sublist, remove_and_match) {
    cmq_sublist_t *sl = cmq_sublist_create();
    cmq_sublist_insert(sl, "foo.bar", (void *)1);
    cmq_sublist_insert(sl, "foo.*", (void *)2);

    cmq_sublist_remove(sl, "foo.bar");

    cmq_sublist_result_t result;
    cmq_sublist_match(sl, "foo.bar", &result);
    ASSERT_EQ(result.count, (size_t)1);
    ASSERT_EQ((intptr_t)result.entries[0], (intptr_t)2);

    cmq_sublist_result_free(&result);
    cmq_sublist_destroy(sl);
}

TEST_MAIN()
