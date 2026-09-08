/* v0.5.166: reload creates the OTel ring when create left it NULL. */
#include "cmq_otel.h"
#include "cmq_test.h"
#include <string.h>

TEST(oea, apply) {
    cmq_otel_t *o = NULL;
    ASSERT_EQ(cmq_otel_reload_attach(&o), 0);
    ASSERT_NOT_NULL(o);
    uint8_t id[16];
    memset(id, 0xab, sizeof(id));
    ASSERT_EQ(cmq_otel_offer(o, id, CMQ_OTEL_KIND_PUBLISH), 0);
    cmq_otel_t *same = o;
    ASSERT_EQ(cmq_otel_reload_attach(&o), 0);
    ASSERT(o == same);
    cmq_otel_destroy(o);
}

TEST(oea, omitted) {
    cmq_otel_t *o = NULL;
    ASSERT_EQ(cmq_otel_reload_attach(&o), 0);
    ASSERT_NOT_NULL(o);
    uint8_t id[16];
    memset(id, 0x11, sizeof(id));
    ASSERT_EQ(cmq_otel_offer(o, id, CMQ_OTEL_KIND_CONNECT), 0);
    cmq_otel_destroy(o);
}

TEST(oea, empty) {
    cmq_otel_t *o = cmq_otel_create();
    ASSERT_NOT_NULL(o);
    cmq_otel_t *same = o;
    ASSERT_EQ(cmq_otel_reload_attach(&o), 0);
    ASSERT(o == same);
    ASSERT_EQ(cmq_otel_reload_attach(&o), 0);
    ASSERT(o == same);
    cmq_otel_destroy(o);
}

TEST(oea, reject) {
    ASSERT(cmq_otel_reload_attach(NULL) != 0);
}

TEST_MAIN()
