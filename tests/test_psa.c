/* v0.5.144: reload attaches persist_dir when create had none. */
#include "cmq_filestore.h"
#include "cmq_test.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

TEST(psa, apply) {
    char tmpl[] = "/tmp/cmq_psa_XXXXXX";
    char *dir = mkdtemp(tmpl);
    ASSERT(dir != NULL);
    cmq_filestore_t *fs = NULL;
    const char *live = NULL;
    ASSERT_EQ(cmq_filestore_reload_attach(&fs, &live, dir), 0);
    ASSERT(fs != NULL);
    ASSERT_STR_EQ(live, dir);
    uint64_t seq = 0;
    ASSERT_EQ(cmq_filestore_append(fs, (const uint8_t *)"hi", 2, &seq), 0);
    ASSERT_EQ(seq, 1u);
    ASSERT_EQ(cmq_filestore_last_seq(fs), 1u);
    cmq_filestore_t *same = fs;
    ASSERT_EQ(cmq_filestore_reload_attach(&fs, &live, "/tmp/cmq_psa_other"), 0);
    ASSERT(fs == same);
    ASSERT_STR_EQ(live, dir);
    cmq_filestore_destroy(fs);
    free((void *)live);
}

TEST(psa, omitted) {
    cmq_filestore_t *fs = NULL;
    const char *live = NULL;
    ASSERT_EQ(cmq_filestore_reload_attach(&fs, &live, NULL), 0);
    ASSERT(fs == NULL);
    ASSERT(live == NULL);
}

TEST(psa, empty) {
    cmq_filestore_t *fs = NULL;
    const char *live = NULL;
    ASSERT_EQ(cmq_filestore_reload_attach(&fs, &live, ""), 0);
    ASSERT(fs == NULL);
}

TEST(psa, reject) {
    cmq_filestore_t *fs = NULL;
    char *live = strdup("/keep");
    ASSERT(cmq_filestore_reload_attach(&fs, (const char **)&live,
                                       "../evil") != 0);
    ASSERT(fs == NULL);
    ASSERT_STR_EQ(live, "/keep");
    ASSERT(cmq_filestore_reload_attach(&fs, (const char **)&live,
                                       "bad\\dir") != 0);
    ASSERT(fs == NULL);
    ASSERT(cmq_filestore_reload_attach(NULL, (const char **)&live,
                                       "/tmp/cmq_psa") != 0);
    free(live);
}

TEST_MAIN()
