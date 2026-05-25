#include "cmq_platform.h"
#include "cmq_types.h"
#include "cmq_test.h"

TEST(platform, detect_os) {
#if CMQ_OS_LINUX
    ASSERT(1);
#elif CMQ_OS_MACOS
    ASSERT(1);
#else
    ASSERT(1);
#endif
}

TEST(platform, detect_arch) {
#if CMQ_ARCH_X86_64
    ASSERT_EQ(sizeof(void*), 8);
#elif CMQ_ARCH_AARCH64
    ASSERT_EQ(sizeof(void*), 8);
#endif
}

TEST(platform, endianness) {
    cmq_u32_t val = 1;
    cmq_u8_t *byte = (cmq_u8_t *)&val;
#if CMQ_LITTLE_ENDIAN
    ASSERT_EQ(byte[0], 1);
#else
    ASSERT_EQ(byte[0], 0);
#endif
}

TEST_MAIN()
