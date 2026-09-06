/* v0.5.143: reload attaches cluster when create had none. */
#include "cmq_cluster.h"
#include "cmq_test.h"
#include <stdlib.h>
#include <string.h>

TEST(cra, apply) {
    cmq_cluster_t *c = NULL;
    const char *name = NULL;
    const char *id = NULL;
    ASSERT_EQ(cmq_cluster_reload_attach(&c, &name, &id, "c1", "n1"), 0);
    ASSERT(c != NULL);
    ASSERT_STR_EQ(name, "c1");
    ASSERT_STR_EQ(id, "n1");
    char out[64];
    ASSERT_EQ(cmq_cluster_name(c, out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "c1");
    ASSERT_EQ(cmq_cluster_self_id(c, out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "n1");
    cmq_cluster_t *same = c;
    ASSERT_EQ(cmq_cluster_reload_attach(&c, &name, &id, "c2", "n2"), 0);
    ASSERT(c == same);
    ASSERT_STR_EQ(name, "c1");
    cmq_cluster_destroy(c);
    free((void *)name);
    free((void *)id);
}

TEST(cra, omitted) {
    cmq_cluster_t *c = NULL;
    const char *name = NULL;
    const char *id = NULL;
    ASSERT_EQ(cmq_cluster_reload_attach(&c, &name, &id, NULL, NULL), 0);
    ASSERT(c == NULL);
}

TEST(cra, empty) {
    cmq_cluster_t *c = NULL;
    const char *name = NULL;
    const char *id = NULL;
    ASSERT_EQ(cmq_cluster_reload_attach(&c, &name, &id, "", ""), 0);
    ASSERT(c == NULL);
}

TEST(cra, reject) {
    cmq_cluster_t *c = NULL;
    char *name = strdup("keep");
    char *id = strdup("id1");
    char longn[80];
    memset(longn, 'a', 64);
    longn[64] = '\0';
    ASSERT(cmq_cluster_reload_attach(&c, (const char **)&name,
                                     (const char **)&id, longn, "n1") != 0);
    ASSERT(c == NULL);
    ASSERT_STR_EQ(name, "keep");
    ASSERT(cmq_cluster_reload_attach(NULL, (const char **)&name,
                                     (const char **)&id, "c1", "n1") != 0);
    free(name);
    free(id);
}

TEST_MAIN()
