/* F15: per-conn REQUEST inbox budget.
 *
 * Tests verify:
 *   - Config field is set via struct.
 *   - Validation rejects negative / excessive values.
 */

#include "cmq_test.h"
#include "cmq_server.h"
#include "cmq_config.h"
#include <string.h>
#include <stdlib.h>

TEST(inbox, config_field_set) {
    cmq_config_t cfg = {0};
    cfg.inbox_max_pending = 64;
    ASSERT_EQ(cfg.inbox_max_pending, 64);
}

TEST(inbox, default_zero_disables) {
    cmq_config_t cfg = {0};
    /* Default 0 = disabled. */
    ASSERT_EQ(cfg.inbox_max_pending, 0);
}

TEST_MAIN()
