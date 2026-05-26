#define _POSIX_C_SOURCE 200809L
#include "cmq_filestore.h"
#include "cmq_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

#define CMQ_FS_MAGIC   0xCF510
#define CMQ_FS_VERSION 1

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint64_t seq;
    uint32_t len;
    uint32_t crc32;
} cmq_fs_record_hdr_t;

struct cmq_filestore {
    char dir[512];
    char prefix[64];
    char data_path[600];
    char idx_path[600];
    FILE *data_fp;
    FILE *idx_fp;
    uint64_t next_seq;
    cmq_mutex_t lock;
};

static uint32_t crc32_compute(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

static void build_paths(cmq_filestore_t *fs) {
    snprintf(fs->data_path, sizeof(fs->data_path), "%s/%s.data", fs->dir, fs->prefix);
    snprintf(fs->idx_path, sizeof(fs->idx_path), "%s/%s.idx", fs->dir, fs->prefix);
}

static uint64_t scan_last_seq(const char *idx_path) {
    FILE *fp = fopen(idx_path, "rb");
    if (!fp) return 0;
    uint64_t last = 0;
    uint64_t offset;
    while (fread(&offset, sizeof(uint64_t), 1, fp) == 1) {
        last++;
    }
    fclose(fp);
    return last;
}

cmq_filestore_t *cmq_filestore_create(const char *dir, const char *prefix) {
    if (!dir || !prefix) return NULL;

    mkdir(dir, 0755);

    cmq_filestore_t *fs = calloc(1, sizeof(cmq_filestore_t));
    if (!fs) return NULL;
    strncpy(fs->dir, dir, sizeof(fs->dir) - 1);
    strncpy(fs->prefix, prefix, sizeof(fs->prefix) - 1);
    cmq_mutex_init(&fs->lock);
    build_paths(fs);

    fs->data_fp = fopen(fs->data_path, "a+b");
    if (!fs->data_fp) { cmq_mutex_destroy(&fs->lock); free(fs); return NULL; }

    fs->idx_fp = fopen(fs->idx_path, "a+b");
    if (!fs->idx_fp) { fclose(fs->data_fp); cmq_mutex_destroy(&fs->lock); free(fs); return NULL; }

    fs->next_seq = scan_last_seq(fs->idx_path) + 1;
    return fs;
}

void cmq_filestore_destroy(cmq_filestore_t *fs) {
    if (!fs) return;
    if (fs->data_fp) fclose(fs->data_fp);
    if (fs->idx_fp) fclose(fs->idx_fp);
    cmq_mutex_destroy(&fs->lock);
    free(fs);
}

int cmq_filestore_append(cmq_filestore_t *fs, const uint8_t *data, size_t len,
                          uint64_t *out_seq) {
    if (!fs || !data || len == 0) return -1;
    cmq_mutex_lock(&fs->lock);

    cmq_fs_record_hdr_t hdr;
    hdr.magic = CMQ_FS_MAGIC;
    hdr.version = CMQ_FS_VERSION;
    hdr.seq = fs->next_seq;
    hdr.len = (uint32_t)len;
    hdr.crc32 = crc32_compute(data, len);

    uint64_t offset = (uint64_t)ftell(fs->data_fp);

    if (fwrite(&hdr, sizeof(hdr), 1, fs->data_fp) != 1 ||
        fwrite(data, 1, len, fs->data_fp) != len) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    fflush(fs->data_fp);

    if (fwrite(&offset, sizeof(uint64_t), 1, fs->idx_fp) != 1) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    fflush(fs->idx_fp);

    if (out_seq) *out_seq = fs->next_seq;
    fs->next_seq++;

    cmq_mutex_unlock(&fs->lock);
    return 0;
}

int cmq_filestore_read(cmq_filestore_t *fs, uint64_t seq,
                        uint8_t **out_data, size_t *out_len) {
    if (!fs || !out_data || !out_len || seq == 0) return -1;
    cmq_mutex_lock(&fs->lock);

    if (seq >= fs->next_seq) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    uint64_t target_idx = seq - 1;
    long idx_pos = (long)(target_idx * sizeof(uint64_t));

    if (fseek(fs->idx_fp, idx_pos, SEEK_SET) != 0) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    uint64_t data_offset;
    if (fread(&data_offset, sizeof(uint64_t), 1, fs->idx_fp) != 1) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    if (fseek(fs->data_fp, (long)data_offset, SEEK_SET) != 0) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    cmq_fs_record_hdr_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, fs->data_fp) != 1) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    if (hdr.magic != CMQ_FS_MAGIC || hdr.seq != seq) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    uint8_t *buf = malloc(hdr.len);
    if (!buf) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    if (fread(buf, 1, hdr.len, fs->data_fp) != hdr.len) {
        free(buf);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    uint32_t crc = crc32_compute(buf, hdr.len);
    if (crc != hdr.crc32) {
        free(buf);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    *out_data = buf;
    *out_len = hdr.len;

    cmq_mutex_unlock(&fs->lock);
    return 0;
}

uint64_t cmq_filestore_last_seq(cmq_filestore_t *fs) {
    if (!fs) return 0;
    cmq_mutex_lock(&fs->lock);
    uint64_t last = fs->next_seq > 0 ? fs->next_seq - 1 : 0;
    cmq_mutex_unlock(&fs->lock);
    return last;
}

int cmq_filestore_sync(cmq_filestore_t *fs) {
    if (!fs) return -1;
    cmq_mutex_lock(&fs->lock);
    int rc = 0;
    if (fs->data_fp) fflush(fs->data_fp);
    if (fs->idx_fp) fflush(fs->idx_fp);
    rc = fsync(fileno(fs->data_fp));
    fsync(fileno(fs->idx_fp));
    cmq_mutex_unlock(&fs->lock);
    return rc;
}
