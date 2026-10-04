/* v0.5.135: reload applies otlp_ca to the live exporter URL. */
#include "cmq_otlp.h"
#include "cmq_test.h"
#include <stdlib.h>
#include <string.h>

TEST(oca, apply) {
    cmq_otlp_url_t u;
    memset(&u, 0, sizeof(u));
    const char *live = NULL;
    ASSERT_EQ(cmq_otlp_reload_ca(&u, &live, "/tmp/cmq_oca.pem"), 0);
    ASSERT_STR_EQ(live, "/tmp/cmq_oca.pem");
    ASSERT_STR_EQ(u.ca, "/tmp/cmq_oca.pem");
    free((void *)live);
}

TEST(oca, omitted) {
    const char *live = NULL;
    ASSERT_EQ(cmq_otlp_reload_ca(NULL, &live, NULL), 0);
    ASSERT(live == NULL);
}

TEST(oca, empty) {
    cmq_otlp_url_t u;
    memset(&u, 0, sizeof(u));
    ASSERT_EQ(cmq_otlp_set_ca(&u, "/old.pem"), 0);
    char *live = strdup("/old.pem");
    ASSERT_EQ(cmq_otlp_reload_ca(&u, (const char **)&live, ""), 0);
    ASSERT_STR_EQ(live, "/old.pem");
    ASSERT_STR_EQ(u.ca, "/old.pem");
    free(live);
}

TEST(oca, reject) {
    cmq_otlp_url_t u;
    memset(&u, 0, sizeof(u));
    ASSERT_EQ(cmq_otlp_set_ca(&u, "/keep.pem"), 0);
    char *live = strdup("/keep.pem");
    ASSERT(cmq_otlp_reload_ca(&u, (const char **)&live, "../evil.pem") != 0);
    ASSERT_STR_EQ(live, "/keep.pem");
    ASSERT_STR_EQ(u.ca, "/keep.pem");
    free(live);
}

TEST_MAIN()
