/* v0.5.149: reload attaches blocklist when create had none. */
#include "cmq_blocklist.h"
#include "cmq_test.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BLA_PATH "build-tdd/bla.txt"

static int write_list(void) {
    (void)system("mkdir -p build-tdd");
    FILE *f = fopen(BLA_PATH, "w");
    if (!f) return -1;
    fputs("10.0.0.1\n", f);
    fclose(f);
    return 0;
}

TEST(bla, apply) {
    ASSERT_EQ(write_list(), 0);
    cmq_blocklist_t *bl = NULL;
    char *live = NULL;
    ASSERT_EQ(cmq_blocklist_reload_attach(&bl, (const char **)&live,
                                          BLA_PATH), 0);
    ASSERT(bl != NULL);
    ASSERT_STR_EQ(live, BLA_PATH);
    ASSERT_EQ(cmq_blocklist_check(bl, htonl(0x0A000001)), 1);
    ASSERT_EQ(cmq_blocklist_check(bl, htonl(0x0A000002)), 0);
    cmq_blocklist_t *same = bl;
    ASSERT_EQ(cmq_blocklist_reload_attach(&bl, (const char **)&live,
                                          "build-tdd/other.txt"), 0);
    ASSERT(bl == same);
    ASSERT_STR_EQ(live, BLA_PATH);
    cmq_blocklist_free(bl);
    free(live);
}

TEST(bla, omitted) {
    cmq_blocklist_t *bl = NULL;
    char *live = NULL;
    ASSERT_EQ(cmq_blocklist_reload_attach(&bl, (const char **)&live, NULL), 0);
    ASSERT(bl == NULL);
}

TEST(bla, empty) {
    cmq_blocklist_t *bl = NULL;
    char *live = NULL;
    ASSERT_EQ(cmq_blocklist_reload_attach(&bl, (const char **)&live, ""), 0);
    ASSERT(bl == NULL);
}

TEST(bla, reject) {
    cmq_blocklist_t *bl = NULL;
    char *live = strdup("/keep.txt");
    ASSERT(cmq_blocklist_reload_attach(&bl, (const char **)&live,
                                       "../evil.txt") != 0);
    ASSERT(bl == NULL);
    ASSERT_STR_EQ(live, "/keep.txt");
    ASSERT(cmq_blocklist_reload_attach(NULL, (const char **)&live,
                                       BLA_PATH) != 0);
    ASSERT(cmq_blocklist_reload_attach(&bl, (const char **)&live,
                                       "build-tdd/missing-bla.txt") != 0);
    ASSERT(bl == NULL);
    free(live);
}

TEST_MAIN()
