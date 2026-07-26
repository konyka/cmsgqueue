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
#include <sys/file.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <time.h>

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
    atomic_int in_flight;
    atomic_int dying;
};

static int fs_begin_op(cmq_filestore_t *fs) {
    if (atomic_load_explicit(&fs->dying, memory_order_acquire))
        return -1;
    atomic_fetch_add_explicit(&fs->in_flight, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&fs->dying, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&fs->in_flight, 1, memory_order_acq_rel);
        return -1;
    }
    return 0;
}

static void fs_end_op(cmq_filestore_t *fs) {
    atomic_fetch_sub_explicit(&fs->in_flight, 1, memory_order_acq_rel);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int)(crc & 1u));
    }
    return crc;
}

static uint32_t crc32_compute(const uint8_t *data, size_t len) {
    return ~crc32_update(0xFFFFFFFFu, data, len);
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

/* Cross-process mutex around data/idx (complements in-process cmq_mutex). */
static int fs_flock(FILE *fp, int op) {
    int fd = fileno(fp);
    if (fd < 0) return -1;
    for (;;) {
        if (flock(fd, op) == 0) return 0;
        if (errno != EINTR) return -1;
    }
}

/* Always lock data then idx (unlock reverse) to cover torn idx reads. */
static int fs_lock_pair(cmq_filestore_t *fs, int op) {
    if (!fs || !fs->data_fp) return -1;
    if (fs_flock(fs->data_fp, op) != 0) return -1;
    if (fs->idx_fp && fs_flock(fs->idx_fp, op) != 0) {
        fs_flock(fs->data_fp, LOCK_UN);
        return -1;
    }
    return 0;
}

static void fs_unlock_pair(cmq_filestore_t *fs) {
    if (!fs) return;
    if (fs->idx_fp) fs_flock(fs->idx_fp, LOCK_UN);
    if (fs->data_fp) fs_flock(fs->data_fp, LOCK_UN);
}

/* Under flock: idx length is the cross-process authority for next_seq.
   may_truncate: only under LOCK_EX — drop a torn trailing partial entry. */
static int fs_refresh_next_seq(cmq_filestore_t *fs, int may_truncate) {
    if (!fs || !fs->idx_fp) return -1;
    if (fs_seek_end(fs->idx_fp) != 0) return -1;
    uint64_t idx_sz;
    if (fs_tell(fs->idx_fp, &idx_sz) != 0) return -1;
    if ((idx_sz % 8u) != 0) {
        idx_sz -= idx_sz % 8u;
        if (may_truncate) {
            if (ftruncate(fileno(fs->idx_fp), (off_t)idx_sz) != 0)
                return -1;
            if (fs_seek_end(fs->idx_fp) != 0) return -1;
        }
    }
    uint64_t n = idx_sz / 8u;
    if (n == UINT64_MAX) return -1; /* cannot form next_seq without wrap */
    fs->next_seq = n + 1;
    return 0;
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

/* dir may be absolute/relative with '/', but no "."/".." components. */
static int dir_safe(const char *dir) {
    if (!dir || !dir[0]) return 0;
    size_t n = strnlen(dir, sizeof(((cmq_filestore_t *)0)->dir));
    if (n == 0 || n >= sizeof(((cmq_filestore_t *)0)->dir)) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)dir[i];
        if (c == '\\' || c < 0x20 || c == 0x7f)
            return 0;
    }
    size_t i = 0;
    while (i < n) {
        while (i < n && dir[i] == '/')
            i++;
        if (i >= n)
            break;
        size_t start = i;
        while (i < n && dir[i] != '/')
            i++;
        size_t len = i - start;
        if (len == 1 && dir[start] == '.')
            return 0;
        if (len == 2 && dir[start] == '.' && dir[start + 1] == '.')
            return 0;
    }
    return 1;
}

/* Drop trailing .data bytes not covered by .idx (crash between fflush(data)
   and idx append). Indexed records stay intact; orphans are truncated.
   Returns 0 on success (incl. nothing to do), -1 on I/O error. */
static int truncate_orphan_data(cmq_filestore_t *fs, uint64_t n_idx) {
    off_t expect = 0;
    if (n_idx > 0) {
        if (fs_seek(fs->idx_fp, (n_idx - 1) * 8u) != 0)
            goto io_fail;
        uint8_t idxb[8];
        if (fread(idxb, sizeof(idxb), 1, fs->idx_fp) != 1)
            goto io_fail;
        uint64_t offset = get_le64(idxb);
        if (fs_seek(fs->data_fp, offset) != 0)
            goto io_fail;
        uint8_t hdr[CMQ_FS_HDR_SIZE];
        if (fread(hdr, sizeof(hdr), 1, fs->data_fp) != 1)
            goto io_fail;
        if (get_le32(hdr) != CMQ_FS_MAGIC)
            return 0;
        uint32_t len = get_le32(hdr + 14);
        expect = (off_t)(offset + (uint64_t)CMQ_FS_HDR_SIZE + (uint64_t)len);
        if (expect < 0 || (uint64_t)expect !=
            offset + (uint64_t)CMQ_FS_HDR_SIZE + (uint64_t)len)
            return 0;
    }
    if (fs_seek_end(fs->data_fp) != 0)
        goto io_fail;
    uint64_t sz;
    if (fs_tell(fs->data_fp, &sz) != 0)
        goto io_fail;
    if ((off_t)sz <= expect)
        return 0;
    if (ftruncate(fileno(fs->data_fp), expect) != 0)
        goto io_fail;
    (void)fs_seek_end(fs->data_fp);
    return 0;
io_fail:
    if (fs->idx_fp) clearerr(fs->idx_fp);
    if (fs->data_fp) clearerr(fs->data_fp);
    return -1;
}

/* Validate idx→data records from the start; truncate at first bad/partial
   entry (crash after idx append with incomplete data, or torn idx write).
   Returns valid count, or UINT64_MAX on I/O failure (do not treat as empty). */
static uint64_t repair_idx(cmq_filestore_t *fs) {
    if (fs_seek_end(fs->idx_fp) != 0)
        return UINT64_MAX;
    uint64_t idx_sz;
    if (fs_tell(fs->idx_fp, &idx_sz) != 0)
        return UINT64_MAX;
    /* Drop torn trailing bytes that are not a full offset entry.
       Fail-closed on truncate/seek — else append's refresh may never write. */
    if ((idx_sz % 8) != 0) {
        idx_sz -= idx_sz % 8;
        if (ftruncate(fileno(fs->idx_fp), (off_t)idx_sz) != 0) {
            clearerr(fs->idx_fp);
            return UINT64_MAX;
        }
        if (fs_seek_end(fs->idx_fp) != 0) {
            clearerr(fs->idx_fp);
            return UINT64_MAX;
        }
    }
    if (fs_seek_end(fs->data_fp) != 0)
        return UINT64_MAX;
    uint64_t data_sz;
    if (fs_tell(fs->data_fp, &data_sz) != 0)
        return UINT64_MAX;

    uint64_t valid = 0;
    uint64_t n = idx_sz / 8u;
    for (uint64_t i = 0; i < n; i++) {
        if (fs_seek(fs->idx_fp, i * 8u) != 0) {
            clearerr(fs->idx_fp);
            return UINT64_MAX;
        }
        uint8_t idxb[8];
        if (fread(idxb, sizeof(idxb), 1, fs->idx_fp) != 1) {
            /* ferror: transient I/O — fail-closed. feof/short: torn/corrupt. */
            if (ferror(fs->idx_fp)) {
                clearerr(fs->idx_fp);
                return UINT64_MAX;
            }
            break;
        }
        uint64_t offset = get_le64(idxb);
        if (offset > UINT64_MAX - (uint64_t)CMQ_FS_HDR_SIZE ||
            offset + (uint64_t)CMQ_FS_HDR_SIZE > data_sz)
            break;
        if (fs_seek(fs->data_fp, offset) != 0) {
            clearerr(fs->data_fp);
            return UINT64_MAX;
        }
        uint8_t hdr[CMQ_FS_HDR_SIZE];
        if (fread(hdr, sizeof(hdr), 1, fs->data_fp) != 1) {
            if (ferror(fs->data_fp)) {
                clearerr(fs->data_fp);
                return UINT64_MAX;
            }
            break;
        }
        if (get_le32(hdr + 0) != CMQ_FS_MAGIC ||
            get_le16(hdr + 4) != (uint16_t)CMQ_FS_VERSION)
            break;
        uint64_t hseq = get_le64(hdr + 6);
        uint32_t hlen = get_le32(hdr + 14);
        if (hseq != i + 1 || hlen == 0 || hlen > (16u * 1024 * 1024))
            break;
        if (offset > UINT64_MAX - (uint64_t)CMQ_FS_HDR_SIZE - (uint64_t)hlen ||
            offset + (uint64_t)CMQ_FS_HDR_SIZE + (uint64_t)hlen > data_sz)
            break;
        /* Stream CRC in fixed chunks — never malloc(hlen); OOM must not
           truncate a still-valid unread suffix of the index. */
        uint32_t crc = 0xFFFFFFFFu;
        uint32_t left = hlen;
        uint8_t chunk[8192];
        int ok = 1;
        while (left > 0) {
            size_t chunk_n = left > (uint32_t)sizeof(chunk) ? sizeof(chunk) : (size_t)left;
            if (fread(chunk, 1, chunk_n, fs->data_fp) != chunk_n) {
                if (ferror(fs->data_fp)) {
                    clearerr(fs->data_fp);
                    return UINT64_MAX;
                }
                ok = 0;
                break;
            }
            crc = crc32_update(crc, chunk, chunk_n);
            left -= (uint32_t)chunk_n;
        }
        if (!ok)
            break;
        uint32_t hcrc = get_le32(hdr + 18);
        if (~crc != hcrc)
            break;
        valid = i + 1;
    }
    if (valid * 8u != idx_sz) {
        /* Must drop the bad suffix before advertising next_seq=valid+1;
           otherwise later refresh_next_seq would re-extend over ghosts. */
        if (ftruncate(fileno(fs->idx_fp), (off_t)(valid * 8u)) != 0) {
            clearerr(fs->idx_fp);
            return UINT64_MAX;
        }
        if (fs_seek_end(fs->idx_fp) != 0) {
            clearerr(fs->idx_fp);
            return UINT64_MAX;
        }
    }
    return valid;
}

cmq_filestore_t *cmq_filestore_create(const char *dir, const char *prefix) {
    if (!dir_safe(dir) || !prefix_safe(prefix)) return NULL;

    mkdir(dir, 0755);

    cmq_filestore_t *fs = calloc(1, sizeof(cmq_filestore_t));
    if (!fs) return NULL;
    snprintf(fs->dir, sizeof(fs->dir), "%s", dir);
    snprintf(fs->prefix, sizeof(fs->prefix), "%s", prefix);
    atomic_init(&fs->in_flight, 0);
    atomic_init(&fs->dying, 0);
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

    uint64_t n = 0;
    int orphan_rc = 0;
    if (fs_lock_pair(fs, LOCK_EX) == 0) {
        n = repair_idx(fs);
        if (n != UINT64_MAX)
            orphan_rc = truncate_orphan_data(fs, n);
        fs_unlock_pair(fs);
    } else {
        /* Fail closed: do not repair unlocked against a live writer. */
        fclose(fs->idx_fp);
        fclose(fs->data_fp);
        cmq_mutex_destroy(&fs->lock);
        free(fs);
        return NULL;
    }
    if (n == UINT64_MAX || orphan_rc != 0) {
        /* repair/orphan I/O failed — never hand out a sticky-ferror handle. */
        fclose(fs->idx_fp);
        fclose(fs->data_fp);
        cmq_mutex_destroy(&fs->lock);
        free(fs);
        return NULL;
    }
    fs->next_seq = n + 1;
    return fs;
}

void cmq_filestore_destroy(cmq_filestore_t *fs) {
    if (!fs) return;
    atomic_store_explicit(&fs->dying, 1, memory_order_release);
    while (atomic_load_explicit(&fs->in_flight, memory_order_acquire) > 0) {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }
    /* Hold lock so concurrent append/get cannot use FILE* mid-fclose. */
    cmq_mutex_lock(&fs->lock);
    if (fs->data_fp) {
        fclose(fs->data_fp);
        fs->data_fp = NULL;
    }
    if (fs->idx_fp) {
        fclose(fs->idx_fp);
        fs->idx_fp = NULL;
    }
    cmq_mutex_unlock(&fs->lock);
    cmq_mutex_destroy(&fs->lock);
    free(fs);
}

static int filestore_append_impl(cmq_filestore_t *fs, const uint8_t *data, size_t len,
                          uint64_t *out_seq) {
    if (!fs || !data || len == 0 || len > (16u * 1024 * 1024)) return -1;
    cmq_mutex_lock(&fs->lock);
    if (!fs->data_fp || !fs->idx_fp) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    if (fs_lock_pair(fs, LOCK_EX) != 0) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    /* Re-sync next_seq from idx — another process may have appended. */
    if (fs_refresh_next_seq(fs, 1) != 0) {
        clearerr(fs->idx_fp);
        clearerr(fs->data_fp);
        fs_unlock_pair(fs);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    /* Refuse when seq cannot advance without wrapping to 0. */
    if (fs->next_seq == UINT64_MAX) {
        fs_unlock_pair(fs);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    /* Always append at EOF — read() leaves the stream mid-file. */
    if (fs_seek_end(fs->data_fp) != 0 || fs_seek_end(fs->idx_fp) != 0) {
        clearerr(fs->idx_fp);
        clearerr(fs->data_fp);
        fs_unlock_pair(fs);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    uint64_t offset;
    if (fs_tell(fs->data_fp, &offset) != 0) {
        clearerr(fs->idx_fp);
        clearerr(fs->data_fp);
        fs_unlock_pair(fs);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    if (offset > UINT64_MAX - (uint64_t)CMQ_FS_HDR_SIZE - (uint64_t)len) {
        fs_unlock_pair(fs);
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
        clearerr(fs->data_fp);
        fs_unlock_pair(fs);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    if (fflush(fs->data_fp) != 0) {
        if (ftruncate(fileno(fs->data_fp), (off_t)offset) == 0)
            fs_seek(fs->data_fp, offset);
        clearerr(fs->data_fp);
        fs_unlock_pair(fs);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    uint8_t idxb[8];
    put_le64(idxb, offset);
    uint64_t idx_off;
    if (fs_tell(fs->idx_fp, &idx_off) != 0) {
        if (ftruncate(fileno(fs->data_fp), (off_t)offset) == 0)
            fs_seek(fs->data_fp, offset);
        clearerr(fs->data_fp);
        fs_unlock_pair(fs);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    if (fwrite(idxb, sizeof(idxb), 1, fs->idx_fp) != 1) {
        fflush(fs->idx_fp);
        /* Only truncate data after idx rollback succeeds — else keep
           data↔idx consistent for repair_idx (avoid ghost→hole). */
        if (ftruncate(fileno(fs->idx_fp), (off_t)idx_off) == 0) {
            fs_seek(fs->idx_fp, idx_off);
            if (ftruncate(fileno(fs->data_fp), (off_t)offset) == 0)
                fs_seek(fs->data_fp, offset);
        }
        clearerr(fs->idx_fp);
        clearerr(fs->data_fp);
        fs_unlock_pair(fs);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    if (fflush(fs->idx_fp) != 0) {
        if (ftruncate(fileno(fs->idx_fp), (off_t)idx_off) == 0) {
            fs_seek(fs->idx_fp, idx_off);
            if (ftruncate(fileno(fs->data_fp), (off_t)offset) == 0)
                fs_seek(fs->data_fp, offset);
        }
        clearerr(fs->idx_fp);
        clearerr(fs->data_fp);
        fs_unlock_pair(fs);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    if (out_seq) *out_seq = fs->next_seq;
    fs->next_seq++;

    fs_unlock_pair(fs);
    cmq_mutex_unlock(&fs->lock);
    return 0;
}

static int filestore_read_impl(cmq_filestore_t *fs, uint64_t seq,
                        uint8_t **out_data, size_t *out_len) {
    if (!fs || !out_data || !out_len || seq == 0) return -1;
    cmq_mutex_lock(&fs->lock);
    if (!fs->data_fp || !fs->idx_fp) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    if (fs_lock_pair(fs, LOCK_SH) != 0) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }

    uint8_t *buf = NULL;
    if (fs_refresh_next_seq(fs, 0) != 0)
        goto fail;

    if (seq >= fs->next_seq)
        goto fail;

    uint64_t target_idx = seq - 1;
    if (target_idx > UINT64_MAX / 8u)
        goto fail;

    if (fs_seek(fs->idx_fp, target_idx * 8u) != 0)
        goto fail;

    uint8_t idxb[8];
    if (fread(idxb, sizeof(idxb), 1, fs->idx_fp) != 1)
        goto fail;
    uint64_t data_offset = get_le64(idxb);

    if (fs_seek_end(fs->data_fp) != 0)
        goto fail;
    uint64_t data_sz;
    if (fs_tell(fs->data_fp, &data_sz) != 0)
        goto fail;
    if (data_offset > UINT64_MAX - (uint64_t)CMQ_FS_HDR_SIZE ||
        data_offset + (uint64_t)CMQ_FS_HDR_SIZE > data_sz)
        goto fail;

    if (fs_seek(fs->data_fp, data_offset) != 0)
        goto fail;

    uint8_t hdr[CMQ_FS_HDR_SIZE];
    if (fread(hdr, sizeof(hdr), 1, fs->data_fp) != 1)
        goto fail;

    uint32_t magic = get_le32(hdr + 0);
    uint16_t version = get_le16(hdr + 4);
    uint64_t hseq = get_le64(hdr + 6);
    uint32_t hlen = get_le32(hdr + 14);
    uint32_t hcrc = get_le32(hdr + 18);

    if (magic != CMQ_FS_MAGIC || version != CMQ_FS_VERSION || hseq != seq)
        goto fail;

    if (hlen == 0 || hlen > (16u * 1024 * 1024))
        goto fail;
    if (data_offset > UINT64_MAX - (uint64_t)CMQ_FS_HDR_SIZE - (uint64_t)hlen ||
        data_offset + (uint64_t)CMQ_FS_HDR_SIZE + (uint64_t)hlen > data_sz)
        goto fail;

    buf = malloc(hlen);
    if (!buf)
        goto fail;

    if (fread(buf, 1, hlen, fs->data_fp) != hlen)
        goto fail;

    if (crc32_compute(buf, hlen) != hcrc)
        goto fail;

    *out_data = buf;
    *out_len = hlen;

    fs_unlock_pair(fs);
    cmq_mutex_unlock(&fs->lock);
    return 0;

fail:
    /* Sticky ferror after a failed fread would poison later appends. */
    if (fs->idx_fp) clearerr(fs->idx_fp);
    if (fs->data_fp) clearerr(fs->data_fp);
    free(buf);
    fs_unlock_pair(fs);
    cmq_mutex_unlock(&fs->lock);
    return -1;
}

static uint64_t filestore_last_seq_impl(cmq_filestore_t *fs) {
    if (!fs) return 0;
    cmq_mutex_lock(&fs->lock);
    uint64_t last = 0;
    if (fs_lock_pair(fs, LOCK_SH) == 0) {
        if (fs_refresh_next_seq(fs, 0) == 0)
            last = fs->next_seq > 0 ? fs->next_seq - 1 : 0;
        else {
            if (fs->idx_fp) clearerr(fs->idx_fp);
            if (fs->data_fp) clearerr(fs->data_fp);
        }
        fs_unlock_pair(fs);
    }
    cmq_mutex_unlock(&fs->lock);
    return last;
}

static int filestore_sync_impl(cmq_filestore_t *fs) {
    if (!fs) return -1;
    cmq_mutex_lock(&fs->lock);
    if (!fs->data_fp || fs_lock_pair(fs, LOCK_EX) != 0) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    int rc = 0;
    if (fflush(fs->data_fp) != 0) rc = -1;
    if (fs->idx_fp && fflush(fs->idx_fp) != 0) rc = -1;
    if (fsync(fileno(fs->data_fp)) != 0) rc = -1;
    if (fs->idx_fp && fsync(fileno(fs->idx_fp)) != 0) rc = -1;
    if (rc != 0) {
        if (fs->idx_fp) clearerr(fs->idx_fp);
        clearerr(fs->data_fp);
    }
    fs_unlock_pair(fs);
    cmq_mutex_unlock(&fs->lock);
    return rc;
}

int cmq_filestore_append(cmq_filestore_t *fs, const uint8_t *data, size_t len,
                          uint64_t *out_seq) {
    if (!fs || !data || len == 0) return -1;
    if (fs_begin_op(fs) != 0) return -1;
    int rc = filestore_append_impl(fs, data, len, out_seq);
    fs_end_op(fs);
    return rc;
}

int cmq_filestore_read(cmq_filestore_t *fs, uint64_t seq,
                        uint8_t **out_data, size_t *out_len) {
    if (!fs) return -1;
    if (fs_begin_op(fs) != 0) return -1;
    int rc = filestore_read_impl(fs, seq, out_data, out_len);
    fs_end_op(fs);
    return rc;
}

uint64_t cmq_filestore_last_seq(cmq_filestore_t *fs) {
    if (!fs) return 0;
    if (fs_begin_op(fs) != 0) return 0;
    uint64_t s = filestore_last_seq_impl(fs);
    fs_end_op(fs);
    return s;
}

int cmq_filestore_sync(cmq_filestore_t *fs) {
    if (!fs) return -1;
    if (fs_begin_op(fs) != 0) return -1;
    int rc = filestore_sync_impl(fs);
    fs_end_op(fs);
    return rc;
}
