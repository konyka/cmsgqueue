#define _POSIX_C_SOURCE 200809L
#include "cmq_store.h"
#include "cmq_stream.h"
#include "cmq_filestore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void demo_memstore(void) {
    printf("=== Memstore Ring Buffer Demo ===\n");
    cmq_store_t *store = cmq_store_create(4);
    if (!store) { printf("Failed to create store\n"); return; }

    uint64_t s1 = cmq_store_put(store, (const uint8_t *)"first", 5);
    uint64_t s2 = cmq_store_put(store, (const uint8_t *)"second", 6);
    uint64_t s3 = cmq_store_put(store, (const uint8_t *)"third", 5);
    printf("Put 3 messages: seq=%llu, %llu, %llu\n",
           (unsigned long long)s1, (unsigned long long)s2, (unsigned long long)s3);

    cmq_store_msg_t msg;
    if (cmq_store_get(store, s1, &msg) == 0)
        printf("Get seq %llu: %.*s (len=%zu)\n", (unsigned long long)s1, (int)msg.len, msg.data, msg.len);
    if (cmq_store_get(store, s2, &msg) == 0)
        printf("Get seq %llu: %.*s (len=%zu)\n", (unsigned long long)s2, (int)msg.len, msg.data, msg.len);

    cmq_store_put(store, (const uint8_t *)"fourth", 6);
    cmq_store_put(store, (const uint8_t *)"fifth", 5);
    printf("After ring overflow (4 slots, 5 puts):\n");
    printf("  seq 1 valid: %s\n", cmq_store_get(store, 1, &msg) == 0 ? "yes" : "no (overwritten)");
    printf("  seq 2 valid: %s\n", cmq_store_get(store, 2, &msg) == 0 ? "yes" : "no (overwritten)");

    cmq_store_destroy(store);
    printf("\n");
}

static void demo_stream(void) {
    printf("=== Stream with Consumers Demo ===\n");
    cmq_stream_t *st = cmq_stream_create("events", 1000, 0);
    if (!st) { printf("Failed to create stream\n"); return; }

    cmq_stream_add_consumer(st, "processor-1");
    cmq_stream_add_consumer(st, "processor-2");

    uint64_t s1 = cmq_stream_append(st, (const uint8_t *)"order.created", 13);
    uint64_t s2 = cmq_stream_append(st, (const uint8_t *)"order.paid", 10);
    printf("Appended 2 messages: seq=%llu, %llu\n",
           (unsigned long long)s1, (unsigned long long)s2);
    printf("Stream: %zu messages, first_seq=%llu\n",
           cmq_stream_msg_count(st),
           (unsigned long long)cmq_stream_first_seq(st));

    cmq_stream_msg_t msg;
    if (cmq_stream_read(st, s1, &msg) == 0)
        printf("Read seq %llu: %.*s\n", (unsigned long long)s1, (int)msg.len, msg.data);

    cmq_stream_consumer_ack(st, "processor-1", s2);
    printf("processor-1 acked up to seq %llu\n", (unsigned long long)s2);

    cmq_stream_consumer_t cs = cmq_stream_consumer_state(st, "processor-1");
    printf("processor-1 state: consumer_seq=%llu, pending=%u\n",
           (unsigned long long)cs.consumer_seq, cs.pending_count);

    cmq_stream_destroy(st);
    printf("\n");
}

static void demo_filestore(void) {
    printf("=== File-based Persistence Demo ===\n");
    const char *dir = "/tmp/cmq_example_fs";
    remove("/tmp/cmq_example_fs/events.data");
    remove("/tmp/cmq_example_fs/events.idx");

    {
        cmq_filestore_t *fs = cmq_filestore_create(dir, "events");
        if (!fs) { printf("Failed to create filestore\n"); return; }

        uint64_t s1, s2;
        cmq_filestore_append(fs, (const uint8_t *)"persistent msg 1", 16, &s1);
        cmq_filestore_append(fs, (const uint8_t *)"persistent msg 2", 16, &s2);
        printf("Appended 2 records: seq=%llu, %llu\n",
               (unsigned long long)s1, (unsigned long long)s2);
        cmq_filestore_sync(fs);

        uint8_t *data = NULL;
        size_t len = 0;
        if (cmq_filestore_read(fs, s1, &data, &len) == 0) {
            printf("Read seq %llu: %.*s (CRC32 verified)\n", (unsigned long long)s1, (int)len, data);
            free(data);
        }

        cmq_filestore_destroy(fs);
    }

    printf("Reopening filestore for crash recovery...\n");
    {
        cmq_filestore_t *fs = cmq_filestore_create(dir, "events");
        if (!fs) { printf("Failed to reopen\n"); return; }
        printf("Last seq after recovery: %llu\n",
               (unsigned long long)cmq_filestore_last_seq(fs));

        uint8_t *data = NULL;
        size_t len = 0;
        if (cmq_filestore_read(fs, 1, &data, &len) == 0) {
            printf("Recovered seq 1: %.*s\n", (int)len, data);
            free(data);
        }
        if (cmq_filestore_read(fs, 2, &data, &len) == 0) {
            printf("Recovered seq 2: %.*s\n", (int)len, data);
            free(data);
        }

        cmq_filestore_destroy(fs);
    }
    printf("\n");
}

int main(void) {
    printf("CMSGQueue Streaming & Persistence Demo\n");
    printf("======================================\n\n");
    demo_memstore();
    demo_stream();
    demo_filestore();
    printf("All demos complete.\n");
    return 0;
}
