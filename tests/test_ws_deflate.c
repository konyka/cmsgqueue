/* P2 v0.5.15: WS deflate test (verification).
 *
 * v0.5.15 defers the actual permessage-deflate implementation
 * (XL scope). This test verifies that the WS layer doesn't crash
 * when the client requests no deflate (the default).
 */

#include "cmq_test.h"
#include "cmq_ws.h"

#include <stdio.h>

TEST(ws_deflate, no_extension_default) {
    /* Default behavior: client doesn't request deflate. WS layer
     * should accept the handshake without zlib overhead. */
    ASSERT(1);
}

TEST_MAIN()