#define _POSIX_C_SOURCE 200809L
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
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
#include <stdint.h>

#define CMQ_FS_MAGIC   0xCF510u
#define CMQ_FS_VERSION 1
#define CMQ_FS_HDR_SIZE 22  /* magic32 + ver16 + seq64 + len32 + crc32, LE */

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

static void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v) {
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t get_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p) {
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

/* Large-file safe seek/tell (avoids 32-bit long truncation via fseek/ftell). */
static int fs_seek(FILE *fp, uint64_t off) {
    off_t o = (off_t)off;
    if (o < 0 || (uint64_t)o != off) return -1;
    return fseeko(fp, o, SEEK_SET);
}

static int fs_seek_end(FILE *fp) {
    return fseeko(fp, 0, SEEK_END);
}

static int fs_tell(FILE *fp, uint64_t *out) {
    off_t o = ftello(fp);
    if (o < 0) return -1;
    *out = (uint64_t)o;
    return 0;
}

static void build_paths(cmq_filestore_t *fs) {
    snprintf(fs->data_path, sizeof(fs->data_path), "%s/%s.data", fs->dir, fs->prefix);
    snprintf(fs->idx_path, sizeof(fs->idx_path), "%s/%s.idx", fs->dir, fs->prefix);
}

/* Prefix is a single path component under dir — reject traversal / separators. */
static int prefix_safe(const char *prefix) {
    if (!prefix || !prefix[0]) return 0;
    size_t n = strnlen(prefix, sizeof(((cmq_filestore_t *)0)->prefix));
    if (n == 0 || n >= sizeof(((cmq_filestore_t *)0)->prefix)) return 0;
    if (strcmp(prefix, ".") == 0 || strcmp(prefix, "..") == 0) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)prefix[i];
        if (c == '/' || c == '\\' || c < 0x20 || c == 0x7f)
            return 0;
    }
    return 1;
}

/* Drop trailing .data bytes not covered by .idx (crash between fflush(data)
   and idx append). Indexed records stay intact; orphans are truncated. */
static void truncate_orphan_data(cmq_filestore_t *fs, uint64_t n_idx) {
    off_t expect = 0;
    if (n_idx > 0) {
        if (fs_seek(fs->idx_fp, (n_idx - 1) * 8u) != 0)
            return;
        uint8_t idxb[8];
        if (fread(idxb, sizeof(idxb), 1, fs->idx_fp) != 1)
            return;
        uint64_t offset = get_le64(idxb);
        if (fs_seek(fs->data_fp, offset) != 0)
            return;
        uint8_t hdr[CMQ_FS_HDR_SIZE];
        if (fread(hdr, sizeof(hdr), 1, fs->data_fp) != 1)
            return;
        if (get_le32(hdr) != CMQ_FS_MAGIC)
            return;
        uint32_t len = get_le32(hdr + 14);
        expect = (off_t)(offset + (uint64_t)CMQ_FS_HDR_SIZE + (uint64_t)len);
        if (expect < 0 || (uint64_t)expect !=
            offset + (uint64_t)CMQ_FS_HDR_SIZE + (uint64_t)len)
            return;
    }
    if (fs_seek_end(fs->data_fp) != 0)
        return;
    uint64_t sz;
    if (fs_tell(fs->data_fp, &sz) != 0 || (off_t)sz <= expect)
        return;
    if (ftruncate(fileno(fs->data_fp), expect) == 0)
        fs_seek_end(fs->data_fp);
}

/* Validate idx→data records from the start; truncate at first bad/partial
   entry (crash after idx append with incomplete data, or torn idx write). */
static uint64_t repair_idx(cmq_filestore_t *fs) {
    if (fs_seek_end(fs->idx_fp) != 0)
        return 0;
    uint64_t idx_sz;
    if (fs_tell(fs->idx_fp, &idx_sz) != 0)
        return 0;
    /* Drop torn trailing bytes that are not a full offset entry. */
    if ((idx_sz % 8) != 0) {
        idx_sz -= idx_sz % 8;
        if (ftruncate(fileno(fs->idx_fp), (off_t)idx_sz) != 0)
            return 0;
    }
    if (fs_seek_end(fs->data_fp) != 0)
        return 0;
    uint64_t data_sz;
    if (fs_tell(fs->data_fp, &data_sz) != 0)
        return 0;

    uint64_t valid = 0;
    uint64_t n = idx_sz / 8u;
    for (uint64_t i = 0; i < n; i++) {
        if (fs_seek(fs->idx_fp, i * 8u) != 0)
            break;
        uint8_t idxb[8];
        if (fread(idxb, sizeof(idxb), 1, fs->idx_fp) != 1)
            break;
        uint64_t offset = get_le64(idxb);
        if (offset + (uint64_t)CMQ_FS_HDR_SIZE > data_sz)
            break;
        if (fs_seek(fs->data_fp, offset) != 0)
            break;
        uint8_t hdr[CMQ_FS_HDR_SIZE];
        if (fread(hdr, sizeof(hdr), 1, fs->data_fp) != 1)
            break;
        if (get_le32(hdr + 0) != CMQ_FS_MAGIC ||
            get_le16(hdr + 4) != (uint16_t)CMQ_FS_VERSION)
            break;
        uint64_t hseq = get_le64(hdr + 6);
        uint32_t hlen = get_le32(hdr + 14);
        if (hseq != i + 1 || hlen == 0 || hlen > (16u * 1024 * 1024))
            break;
        if (offset + (uint64_t)CMQ_FS_HDR_SIZE + (uint64_t)hlen > data_sz)
            break;
        uint8_t *buf = malloc(hlen);
        if (!buf)
            break;
        if (fread(buf, 1, hlen, fs->data_fp) != hlen) {
            free(buf);
            break;
        }
        uint32_t hcrc = get_le32(hdr + 18);
        int ok = (crc32_compute(buf, hlen) == hcrc);
        free(buf);
        if (!ok)
            break;
        valid = i + 1;
    }
    if (valid * 8u != idx_sz) {
        if (ftruncate(fileno(fs->idx_fp), (off_t)(valid * 8u)) == 0)
            fs_seek_end(fs->idx_fp);
    }
    return valid;
}

cmq_filestore_t *cmq_filestore_create(const char *dir, const char *prefix) {
    if (!dir || !prefix_safe(prefix)) return NULL;
    if (strnlen(dir, sizeof(((cmq_filestore_t *)0)->dir)) >=
        sizeof(((cmq_filestore_t *)0)->dir))
        return NULL;

    mkdir(dir, 0755);

    cmq_filestore_t *fs = calloc(1, sizeof(cmq_filestore_t));
    if (!fs) return NULL;
    strncpy(fs->dir, dir, sizeof(fs->dir) - 1);
    strncpy(fs->prefix, prefix, sizeof(fs->prefix) - 1);
    cmq_mutex_init(&fs->lock);
    int dlen = snprintf(fs->data_path, sizeof(fs->data_path), "%s/%s.data",
                        fs->dir, fs->prefix);
    int ilen = snprintf(fs->idx_path, sizeof(fs->idx_path), "%s/%s.idx",
                        fs->dir, fs->prefix);
    if (dlen < 0 || (size_t)dlen >= sizeof(fs->data_path) ||
        ilen < 0 || (size_t)ilen >= sizeof(fs->idx_path)) {
        cmq_mutex_destroy(&fs->lock);
        free(fs);
        return NULL;
    }

    fs->data_fp = fopen(fs->data_path, "a+b");
    if (!fs->data_fp) { cmq_mutex_destroy(&fs->lock); free(fs); return NULL; }

    fs->idx_fp = fopen(fs->idx_path, "a+b");
    if (!fs->idx_fp) { fclose(fs->data_fp); cmq_mutex_destroy(&fs->lock); free(fs); return NULL; }

    uint64_t n = repair_idx(fs);
    truncate_orphan_data(fs, n);
    fs->next_seq = n + 1;
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
    if (!fs || !data || len == 0 || len > (16u * 1024 * 1024)) return -1;
    cmq_mutex_lock(&fs->lock);

    /* Always append at EOF — read() leaves the stream mid-file. */
    if (fs_seek_end(fs->data_fp) != 0 || fs_seek_end(fs->idx_fp) != 0) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    uint64_t offset;
    if (fs_tell(fs->data_fp, &offset) != 0) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    if (offset > UINT64_MAX - (uint64_t)CMQ_FS_HDR_SIZE - (uint64_t)len) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    uint8_t hdr[CMQ_FS_HDR_SIZE];
    put_le32(hdr + 0, CMQ_FS_MAGIC);
    put_le16(hdr + 4, (uint16_t)CMQ_FS_VERSION);
    put_le64(hdr + 6, fs->next_seq);
    put_le32(hdr + 14, (uint32_t)len);
    put_le32(hdr + 18, crc32_compute(data, len));

    if (fwrite(hdr, sizeof(hdr), 1, fs->data_fp) != 1 ||
        fwrite(data, 1, len, fs->data_fp) != len) {
        fflush(fs->data_fp);
        if (ftruncate(fileno(fs->data_fp), (off_t)offset) == 0)
            fs_seek(fs->data_fp, offset);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    if (fflush(fs->data_fp) != 0) {
        if (ftruncate(fileno(fs->data_fp), (off_t)offset) == 0)
            fs_seek(fs->data_fp, offset);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    uint8_t idxb[8];
    put_le64(idxb, offset);
    uint64_t idx_off;
    if (fs_tell(fs->idx_fp, &idx_off) != 0) {
        if (ftruncate(fileno(fs->data_fp), (off_t)offset) == 0)
            fs_seek(fs->data_fp, offset);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    if (fwrite(idxb, sizeof(idxb), 1, fs->idx_fp) != 1) {
        fflush(fs->idx_fp);
        if (ftruncate(fileno(fs->data_fp), (off_t)offset) == 0)
            fs_seek(fs->data_fp, offset);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    if (fflush(fs->idx_fp) != 0) {
        ftruncate(fileno(fs->idx_fp), (off_t)idx_off);
        if (ftruncate(fileno(fs->data_fp), (off_t)offset) == 0)
            fs_seek(fs->data_fp, offset);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

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
    if (target_idx > UINT64_MAX / 8u) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    if (fs_seek(fs->idx_fp, target_idx * 8u) != 0) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    uint8_t idxb[8];
    if (fread(idxb, sizeof(idxb), 1, fs->idx_fp) != 1) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    uint64_t data_offset = get_le64(idxb);

    if (fs_seek_end(fs->data_fp) != 0) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    uint64_t data_sz;
    if (fs_tell(fs->data_fp, &data_sz) != 0) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    if (data_offset + (uint64_t)CMQ_FS_HDR_SIZE > data_sz) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    if (fs_seek(fs->data_fp, data_offset) != 0) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    uint8_t hdr[CMQ_FS_HDR_SIZE];
    if (fread(hdr, sizeof(hdr), 1, fs->data_fp) != 1) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    uint32_t magic = get_le32(hdr + 0);
    uint16_t version = get_le16(hdr + 4);
    uint64_t hseq = get_le64(hdr + 6);
    uint32_t hlen = get_le32(hdr + 14);
    uint32_t hcrc = get_le32(hdr + 18);

    if (magic != CMQ_FS_MAGIC || version != CMQ_FS_VERSION || hseq != seq) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    if (hlen == 0 || hlen > (16u * 1024 * 1024)) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    if (data_offset + (uint64_t)CMQ_FS_HDR_SIZE + (uint64_t)hlen > data_sz) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    uint8_t *buf = malloc(hlen);
    if (!buf) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    if (fread(buf, 1, hlen, fs->data_fp) != hlen) {
        free(buf);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    if (crc32_compute(buf, hlen) != hcrc) {
        free(buf);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    *out_data = buf;
    *out_len = hlen;

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
    if (fs->data_fp && fflush(fs->data_fp) != 0) rc = -1;
    if (fs->idx_fp && fflush(fs->idx_fp) != 0) rc = -1;
    if (fs->data_fp && fsync(fileno(fs->data_fp)) != 0) rc = -1;
    if (fs->idx_fp && fsync(fileno(fs->idx_fp)) != 0) rc = -1;
    cmq_mutex_unlock(&fs->lock);
    return rc;
}
