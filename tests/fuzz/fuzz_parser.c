/* F20: Minimal libfuzzer harness for cmq_parser_feed.
 *
 * Compile: clang -fsanitize=fuzzer,fuzzer-no-link,address
 *          tests/fuzz/fuzz_parser.c src/proto/cmq_parser.c ...
 *          -o fuzz_parser
 *
 * Run: ./fuzz_parser corpus/
 *
 * The harness feeds random bytes into the parser and ensures it
 * does not crash, OOB-read, or leak. Stateful: each call clears
 * the parser to start fresh.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

extern void *cmq_parser_create(void);
extern void cmq_parser_destroy(void *p);
extern int cmq_parser_set_max_payload(void *p, size_t cap);
extern int cmq_parser_feed(void *p, const uint8_t *data, size_t len);
extern int cmq_parser_pending_error(void *p);
extern const void *cmq_parser_frame(void *p);
extern int cmq_parser_next(void *p);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65536) return 0; /* bound input */
    void *p = cmq_parser_create();
    if (!p) return 0;
    cmq_parser_set_max_payload(p, 16 * 1024 * 1024);
    /* Feed in two chunks to exercise the partial-frame path. */
    size_t mid = size / 2;
    cmq_parser_feed(p, data, mid);
    if (mid < size) cmq_parser_feed(p, data + mid, size - mid);
    /* Drain any frames the parser produced. */
    while (cmq_parser_next(p) == 1) { /* keep going */ }
    (void)cmq_parser_pending_error(p);
    cmq_parser_destroy(p);
    return 0;
}
