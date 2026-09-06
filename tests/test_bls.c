/* v0.5.154: reload swaps blocklist and copies the live path. */
#include "cmq_blocklist.h"
#include "cmq_test.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BLS_OLD "build-tdd/bls_old.txt"
#define BLS_NEW "build-tdd/bls_new.txt"

static int write_one(const char *path, const char *line) {
    (void)system("mkdir -p build-tdd");
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(line, f);
    fclose(f);
    return 0;
}

TEST(bls, apply) {
    ASSERT_EQ(write_one(BLS_OLD, "10.0.0.1\n"), 0);
    ASSERT_EQ(write_one(BLS_NEW, "10.0.0.2\n"), 0);
    cmq_blocklist_t *old = cmq_blocklist_load(BLS_OLD);
    ASSERT(old != NULL);
    char *live = strdup(BLS_OLD);
    cmq_blocklist_t *neu = NULL;
    ASSERT_EQ(cmq_blocklist_reload_swap(&neu, (const char **)&live,
                                        BLS_NEW), 0);
    ASSERT(neu != NULL);
    ASSERT_STR_EQ(live, BLS_NEW);
    ASSERT_EQ(cmq_blocklist_check(neu, htonl(0x0A000002)), 1);
    ASSERT_EQ(cmq_blocklist_check(neu, htonl(0x0A000001)), 0);
    cmq_blocklist_free(neu);
    neu = NULL;
    ASSERT_EQ(cmq_blocklist_reload_swap(&neu, (const char **)&live,
                                        BLS_NEW), 0);
    ASSERT_STR_EQ(live, BLS_NEW);
    cmq_blocklist_free(old);
    cmq_blocklist_free(neu);
    free(live);
}

TEST(bls, omitted) {
    cmq_blocklist_t *neu = NULL;
    char *live = strdup("/keep.txt");
    ASSERT_EQ(cmq_blocklist_reload_swap(&neu, (const char **)&live, NULL), 0);
    ASSERT(neu == NULL);
    ASSERT_STR_EQ(live, "/keep.txt");
    free(live);
}

TEST(bls, empty) {
    cmq_blocklist_t *neu = NULL;
    char *live = strdup("/keep.txt");
    ASSERT_EQ(cmq_blocklist_reload_swap(&neu, (const char **)&live, ""), 0);
    ASSERT(neu == NULL);
    ASSERT_STR_EQ(live, "/keep.txt");
    free(live);
}

TEST(bls, reject) {
    cmq_blocklist_t *neu = NULL;
    char *live = strdup("/keep.txt");
    ASSERT(cmq_blocklist_reload_swap(&neu, (const char **)&live,
                                     "../evil.txt") != 0);
    ASSERT(neu == NULL);
    ASSERT_STR_EQ(live, "/keep.txt");
    ASSERT(cmq_blocklist_reload_swap(NULL, (const char **)&live,
                                     BLS_NEW) != 0);
    ASSERT(cmq_blocklist_reload_swap(&neu, (const char **)&live,
                                     "build-tdd/missing-bls.txt") != 0);
    ASSERT(neu == NULL);
    ASSERT_STR_EQ(live, "/keep.txt");
    ASSERT(cmq_blocklist_reload_swap(&neu, (const char **)&live,
                                     "bad\\path.txt") != 0);
    ASSERT_STR_EQ(live, "/keep.txt");
    free(live);
}

TEST_MAIN()
