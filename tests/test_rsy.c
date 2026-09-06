/* v0.5.121: reload applies persist_sync_interval_ms. */
#include "cmq_filestore.h"
#include "cmq_test.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

TEST(rsy, apply) {
    const char *dir = "/tmp/cmq_rsy_apply";
    (void)mkdir(dir, 0755);
    cmq_filestore_t *fs = cmq_filestore_create(dir, "rsy");
    ASSERT(fs != NULL);
    cmq_filestore_set_sync_interval(fs, 1000);
    unsigned live = 1000;
    ASSERT_EQ(cmq_filestore_reload_sync(fs, &live, 5000), 0);
    ASSERT_EQ(live, 5000u);
    ASSERT_EQ(cmq_filestore_sync_interval(fs), 5000u);
    cmq_filestore_destroy(fs);
}

TEST(rsy, omitted) {
    unsigned live = 2500;
    ASSERT_EQ(cmq_filestore_reload_sync(NULL, &live, 0), 0);
    ASSERT_EQ(live, 2500u);
}

TEST(rsy, empty) {
    const char *dir = "/tmp/cmq_rsy_empty";
    (void)mkdir(dir, 0755);
    cmq_filestore_t *fs = cmq_filestore_create(dir, "rsy");
    ASSERT(fs != NULL);
    cmq_filestore_set_sync_interval(fs, 1500);
    unsigned live = 1500;
    ASSERT_EQ(cmq_filestore_reload_sync(fs, &live, 0), 0);
    ASSERT_EQ(live, 1500u);
    ASSERT_EQ(cmq_filestore_sync_interval(fs), 1500u);
    cmq_filestore_destroy(fs);
}

TEST(rsy, reject) {
    unsigned live = 800;
    ASSERT(cmq_filestore_reload_sync(NULL, &live, 86400001u) != 0);
    ASSERT_EQ(live, 800u);
    ASSERT(cmq_filestore_reload_sync(NULL, NULL, 1000) != 0);
}

TEST_MAIN()
