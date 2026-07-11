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

TEST(sublist, insert_duplicate_subject) {
    cmq_sublist_t *sl = cmq_sublist_create();
    cmq_sublist_insert(sl, "foo.bar", (void *)1);
    int rc = cmq_sublist_insert(sl, "foo.bar", (void *)2);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(cmq_sublist_count(sl), (size_t)2);

    cmq_sublist_result_t result;
    cmq_sublist_match(sl, "foo.bar", &result);
    ASSERT_EQ(result.count, (size_t)2);
    ASSERT_EQ((intptr_t)result.entries[0], (intptr_t)1);
    ASSERT_EQ((intptr_t)result.entries[1], (intptr_t)2);

    cmq_sublist_result_free(&result);
    cmq_sublist_destroy(sl);
}

TEST(sublist, remove_basic) {
    cmq_sublist_t *sl = cmq_sublist_create();
    cmq_sublist_insert(sl, "foo.bar", (void *)1);
    int rc = cmq_sublist_remove(sl, "foo.bar", (void *)1);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(cmq_sublist_count(sl), (size_t)0);
    cmq_sublist_destroy(sl);
}

TEST(sublist, remove_exact_of_duplicates) {
    cmq_sublist_t *sl = cmq_sublist_create();
    cmq_sublist_insert(sl, "foo.bar", (void *)1);
    cmq_sublist_insert(sl, "foo.bar", (void *)2);
    cmq_sublist_insert(sl, "foo.bar", (void *)3);
    ASSERT_EQ(cmq_sublist_remove(sl, "foo.bar", (void *)2), 0);
    ASSERT_EQ(cmq_sublist_count(sl), (size_t)2);

    cmq_sublist_result_t result;
    cmq_sublist_match(sl, "foo.bar", &result);
    ASSERT_EQ(result.count, (size_t)2);
    int saw1 = 0, saw3 = 0, saw2 = 0;
    for (size_t i = 0; i < result.count; i++) {
        intptr_t v = (intptr_t)result.entries[i];
        if (v == 1) saw1 = 1;
        if (v == 2) saw2 = 1;
        if (v == 3) saw3 = 1;
    }
    ASSERT_EQ(saw1, 1);
    ASSERT_EQ(saw3, 1);
    ASSERT_EQ(saw2, 0);
    cmq_sublist_result_free(&result);
    cmq_sublist_destroy(sl);
}

TEST(sublist, remove_nonexistent) {
    cmq_sublist_t *sl = cmq_sublist_create();
    int rc = cmq_sublist_remove(sl, "nope", (void *)1);
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

TEST(sublist, invalid_subject_too_many_tokens) {
    cmq_sublist_t *sl = cmq_sublist_create();
    char subject[512];
    size_t off = 0;
    for (int i = 0; i < 65; i++) {
        if (i) subject[off++] = '.';
        subject[off++] = 'a';
    }
    subject[off] = '\0';
    int rc = cmq_sublist_insert(sl, subject, (void *)1);
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

    cmq_sublist_remove(sl, "foo.bar", (void *)1);

    cmq_sublist_result_t result;
    cmq_sublist_match(sl, "foo.bar", &result);
    ASSERT_EQ(result.count, (size_t)1);
    ASSERT_EQ((intptr_t)result.entries[0], (intptr_t)2);

    cmq_sublist_result_free(&result);
    cmq_sublist_destroy(sl);
}

TEST(sublist, subject_valid_api) {
    ASSERT_EQ(cmq_sublist_subject_valid("foo.bar"), 0);
    ASSERT_EQ(cmq_sublist_subject_valid("foo..bar"), -1);
    ASSERT_EQ(cmq_sublist_subject_valid(".foo"), -1);
    ASSERT_EQ(cmq_sublist_subject_valid("foo."), -1);
    ASSERT_EQ(cmq_sublist_subject_valid(""), -1);
    ASSERT_EQ(cmq_sublist_subject_valid("foo.*"), 0);
    ASSERT_EQ(cmq_sublist_subject_valid(">"), 0);
    ASSERT_EQ(cmq_sublist_publish_subject_valid("foo.bar"), 0);
    ASSERT_EQ(cmq_sublist_publish_subject_valid("foo.*"), -1);
    ASSERT_EQ(cmq_sublist_publish_subject_valid(">"), -1);
    ASSERT_EQ(cmq_sublist_publish_subject_valid("foo.>"), -1);
}

TEST_MAIN()
