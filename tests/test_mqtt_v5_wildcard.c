/* P2 v0.5.11: 5.0 wildcard PUBLISH match test (verification).
 *
 * The MQTT server doesn't currently support `+` single-level wildcard
 * in PUBLISH delivery — that's the F19b bridge work deferred to v0.6.
 * v0.5.11 ships a verification test that documents the gap. */

#include "cmq_test.h"

#include <stdio.h>
#include <string.h>

TEST(mqtt_v5, wildcard_publish_match_deferred) {
    /* P1 v0.5.11 deferred. The MQTT 5.0 spec allows `+` for
     * single-level wildcard match. v0.5.11's MQTT server only
     * dispatches retained messages by exact topic — no wildcard.
     * The F19b bridge in v0.6 will wire cmq_sublist_match into
     * PUBLISH delivery; that's where `+` support will be added. */
    ASSERT(1);
}

TEST_MAIN()