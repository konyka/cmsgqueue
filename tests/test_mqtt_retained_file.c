/* v0.5.84: MQTT retain file format roundtrip defensive test.
 *
 * The retained-message store (cmq_mqtt_store_retained) has a
 * persistent file mode enabled via cmq_mqtt_set_retain_path.
 * v0.5.43 covered the in-memory dispatch (wildcard subscriber
 * matching) but not the file format. v0.5.84 closes that gap.
 *
 * Two tests:
 *   1. mqtt_retained_file, file_format_text
 *      Stores a retained message with a unique topic prefix,
 *      then inspects the on-disk file. Each line must be
 *      "<topic> <len> <payload>\n" with a single length
 *      field. Catches regressions where the writer
 *      reintroduces a duplicate length.
 *   2. mqtt_retained_file, roundtrip_via_set_path
 *      Stores a message, then re-calls cmq_mqtt_set_retain_path
 *      to simulate a server restart (file is re-read on path
 *      set). Verifies the message comes back via
 *      cmq_mqtt_fetch_retained with the same payload.
 *
 * v0.5.84 also cleaned up the writer/reader in
 * cmq_mqtt_server.c — the OLD format had a redundant
 * `(int)payload_len` and `len` both written (always the
 * same value). The new format writes one length field.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmq_test.h"
#include "cmq_mqtt_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#define V0584_RETAIN_PATH_FMT "/tmp/cmq-test-v0584-retain-%d-%d"

/* Generate a process-unique file path. */
static void v0584_unique_path(char *out, size_t out_len) {
    snprintf(out, out_len, V0584_RETAIN_PATH_FMT,
             (int)getpid(), (int)time(NULL));
}

TEST(mqtt_retained_file, file_format_text) {
    char path[256];
    v0584_unique_path(path, sizeof(path));
    /* Start fresh. */
    unlink(path);
    cmq_mqtt_set_retain_path(path);

    /* Store a retained message with a unique topic prefix so we
     * don't collide with other tests' state. */
    const char *topic = "v0584/filefmt/test";
    const uint8_t payload[] = "hello-format";
    cmq_mqtt_store_retained(topic, payload, sizeof(payload) - 1);

    /* Inspect the file directly. */
    FILE *f = fopen(path, "r");
    ASSERT_NOT_NULL(f);

    /* Read line 1. Must be: "<topic> <len> <payload>\n" */
    char buf[1024];
    char *line = fgets(buf, sizeof(buf), f);
    ASSERT_NOT_NULL(line);

    /* The line should start with the topic followed by a
     * space, then a single length field (a digit), then
     * a space, then the payload, then a newline. */
    /* Find the second space (delimiter between length and
     * payload). If the writer reintroduces a duplicate
     * length, the second space will be after a SECOND digit. */
    int topic_len = (int)strlen(topic);
    ASSERT_EQ(strncmp(line, topic, topic_len), 0);
    ASSERT(line[topic_len] == ' ');
    /* After topic + space, expect a digit (length). */
    char *after_len = line + topic_len + 1;
    int digits = 0;
    while (after_len[digits] >= '0' && after_len[digits] <= '9') digits++;
    ASSERT(digits > 0);
    /* Right after the digits, expect a space and the payload. */
    ASSERT(after_len[digits] == ' ');
    /* The next bytes should be the payload. */
    ASSERT_EQ(strcmp(after_len + digits + 1, "hello-format\n"), 0);

    fclose(f);
    unlink(path);
}

TEST(mqtt_retained_file, roundtrip_via_set_path) {
    char path[256];
    v0584_unique_path(path, sizeof(path));
    unlink(path);
    cmq_mqtt_set_retain_path(path);

    /* Store a retained message. */
    const char *topic = "v0584/roundtrip/test";
    const uint8_t payload[] = "roundtrip-payload-v0584";
    cmq_mqtt_store_retained(topic, payload, sizeof(payload) - 1);

    /* Fetch the in-memory copy. */
    const uint8_t *mem_payload = NULL;
    size_t mem_len = 0;
    int fetch_rc = cmq_mqtt_fetch_retained(topic, &mem_payload, &mem_len);
    ASSERT_EQ(fetch_rc, 0);
    ASSERT_EQ(mem_len, sizeof(payload) - 1);
    ASSERT_EQ(memcmp(mem_payload, payload, mem_len), 0);

    /* Re-read the file. This simulates a server restart:
     * cmq_mqtt_set_retain_path opens the file and calls
     * cmq_mqtt_store_retained for each entry. The new entry
     * is appended to g_mqtt_retained (the existing one is
     * replaced because the topic matches). */
    cmq_mqtt_set_retain_path(path);

    /* Fetch again. The entry should still be there. */
    const uint8_t *disk_payload = NULL;
    size_t disk_len = 0;
    fetch_rc = cmq_mqtt_fetch_retained(topic, &disk_payload, &disk_len);
    ASSERT_EQ(fetch_rc, 0);
    ASSERT_EQ(disk_len, sizeof(payload) - 1);
    ASSERT_EQ(memcmp(disk_payload, payload, disk_len), 0);

    unlink(path);
}

TEST(mqtt_retained_file, truncated_record_does_not_poison_following) {
    char path[256];
    v0584_unique_path(path, sizeof(path));
    unlink(path);

    FILE *f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    /* The first record declares a payload larger than the bounded
     * retained-record limit and has no payload bytes. */
    fputs("v0585/truncated/test 1000000 \n", f);
    const uint8_t payload[] = "survives-truncation";
    fprintf(f, "v0585/valid/test %zu ", sizeof(payload) - 1);
    fwrite(payload, 1, sizeof(payload) - 1, f);
    fputc('\n', f);
    fclose(f);

    cmq_mqtt_set_retain_path(path);

    const uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ(cmq_mqtt_fetch_retained("v0585/truncated/test",
                                      &out, &out_len), -1);
    ASSERT_EQ(cmq_mqtt_fetch_retained("v0585/valid/test",
                                      &out, &out_len), 0);
    ASSERT_EQ(out_len, sizeof(payload) - 1);
    ASSERT_EQ(memcmp(out, payload, out_len), 0);

    unlink(path);
}

/* v0.5.86: same-topic replace contract.
 *
 * Lock in the last-write-wins replacement contract for
 * retained-file recovery. cmq_mqtt_store_retained searches
 * the in-memory array for an existing entry with the same
 * topic and replaces it. When the file is read in order,
 * the LAST entry in the file must win. A future regression
 * that appends without checking for an existing match would
 * let the first entry leak into the retained list and
 * silently consume the MQTT_MAX_RETAINED cap.
 */
TEST(mqtt_retained_file, same_topic_second_entry_wins) {
    char path[256];
    v0584_unique_path(path, sizeof(path));
    unlink(path);

    FILE *f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    /* Two records for the same topic, in order. The second
     * payload must win on recovery. */
    const char *shared_topic = "v0586/dup/test";
    const uint8_t first_payload[] = "first-payload";
    const uint8_t second_payload[] = "second-payload-later-wins";
    fprintf(f, "%s %zu ", shared_topic, sizeof(first_payload) - 1);
    fwrite(first_payload, 1, sizeof(first_payload) - 1, f);
    fputc('\n', f);
    fprintf(f, "%s %zu ", shared_topic, sizeof(second_payload) - 1);
    fwrite(second_payload, 1, sizeof(second_payload) - 1, f);
    fputc('\n', f);
    fclose(f);

    cmq_mqtt_set_retain_path(path);

    const uint8_t *out = NULL;
    size_t out_len = 0;
    int fetch_rc = cmq_mqtt_fetch_retained(shared_topic, &out, &out_len);
    ASSERT_EQ(fetch_rc, 0);
    ASSERT_EQ(out_len, sizeof(second_payload) - 1);
    /* The recovered payload must be the SECOND entry, not
     * the first. A regression that returned "first-payload"
     * here would let stale retained messages survive a
     * reload. */
    if (out_len == sizeof(second_payload) - 1 &&
        memcmp(out, second_payload, out_len) == 0) {
        /* expected */
    } else {
        fprintf(stderr, "v0.5.86: same-topic retained was '%.*s' "
                "(expected '%s')\n", (int)out_len,
                out ? (const char *)out : "", second_payload);
    }
    ASSERT_EQ(memcmp(out, second_payload, out_len), 0);

    unlink(path);
}

/* The test runner is supplied by cmq_test.h. */
TEST_MAIN()
