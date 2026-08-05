/* F18: Persistent sublist stub test. */

#include "cmq_test.h"
#include "cmq_sublist_persist.h"

TEST(sublist_persist, open_returns_null_until_implemented) {
    cmq_sublist_persist_t *p = cmq_sublist_persist_open("/tmp");
    /* Stub returns NULL until full implementation lands. */
    ASSERT(p == NULL);
}

TEST(sublist_persist, record_returns_error) {
    cmq_sublist_persist_t *p = NULL;
    int rc = cmq_sublist_persist_record_sub(p, 1, "foo", "user1");
    ASSERT(rc == -1);
    rc = cmq_sublist_persist_record_unsub(p, 1);
    ASSERT(rc == -1);
    rc = cmq_sublist_persist_load(p, NULL, NULL);
    ASSERT(rc == -1);
}

TEST_MAIN()
