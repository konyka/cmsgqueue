#define _POSIX_C_SOURCE 200809L
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "cmq_filestore.h"
#include "cmq_thread.h"
#include "cmq_atomic.h"

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
    /* P2: cached EOF offsets so the hot-path append doesn't need
     * seek_end+ftello before every write. Updated after each successful
     * append under the lock; reset when fs_refresh_next_seq runs. */
    uint64_t data_end_off;
    uint64_t idx_end_off;
    cmq_mutex_t lock;
    atomic_int in_flight;
    atomic_int dying;
    /* P2 (v0.5.3): stat counter for async-enqueue blocks. */
    cmq_atomic_u64 *async_blocked;
    /* P3 v0.5.4: successful async enqueue counter (vs blocked). */
    cmq_atomic_u64 *async_enqueued;
    /* P3: periodic fsync policy. */
    unsigned fsync_interval_ms;
    uint64_t last_sync_ms;
    /* P1 v0.5.5: max payload size for async enqueue. */
    size_t max_payload_bytes;
    /* P1: async WAL ring (SPSC + worker thread). The producer enqueues
     * a copy of (data, len); the worker drains and writes. */
    pthread_t async_thread;
    int async_active;
    unsigned async_capacity;
    unsigned async_head;       /* producer writes here */
    unsigned async_tail;       /* consumer reads here */
    unsigned async_count;      /* in-flight entries */
    pthread_mutex_t async_lock; /* protects ring */
    pthread_cond_t async_not_empty;
    pthread_cond_t async_not_full;
    uint8_t **async_entries;    /* each: hdr+payload, allocated by producer */
    size_t *async_lens;
    uint64_t *async_seqs;
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
    fs->async_blocked = calloc(1, sizeof(cmq_atomic_u64));
    fs->async_enqueued = calloc(1, sizeof(cmq_atomic_u64));
    if (!fs->async_blocked || !fs->async_enqueued) {
        cmq_mutex_destroy(&fs->lock);
        free(fs->async_blocked);
        free(fs->async_enqueued);
        free(fs);
        return NULL;
    }
    /* P1 v0.5.5: default 1 MiB cap, configurable later. */
    fs->max_payload_bytes = 1u * 1024u * 1024u;
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
    if (!fs->data_fp) {
        cmq_mutex_destroy(&fs->lock);
        free(fs->async_blocked); free(fs);
        return NULL;
    }

    fs->idx_fp = fopen(fs->idx_path, "a+b");
    if (!fs->idx_fp) {
        fclose(fs->data_fp);
        cmq_mutex_destroy(&fs->lock);
        free(fs->async_blocked); free(fs);
        return NULL;
    }

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
        free(fs->async_blocked);
        free(fs);
        return NULL;
    }
    fs->next_seq = n + 1;
    return fs;
}

void cmq_filestore_destroy(cmq_filestore_t *fs) {
    if (!fs) return;
    atomic_store_explicit(&fs->dying, 1, memory_order_release);
    if (fs->async_active) {
        pthread_mutex_lock(&fs->async_lock);
        pthread_cond_broadcast(&fs->async_not_empty);
        pthread_mutex_unlock(&fs->async_lock);
        pthread_join(fs->async_thread, NULL);
        pthread_mutex_destroy(&fs->async_lock);
        pthread_cond_destroy(&fs->async_not_empty);
        pthread_cond_destroy(&fs->async_not_full);
        /* Free any leftover ring entries. */
        for (unsigned i = 0; i < fs->async_count; i++) {
            free(fs->async_entries[(fs->async_tail + i) % fs->async_capacity]);
        }
        free(fs->async_entries);
        free(fs->async_lens);
        free(fs->async_seqs);
    }
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
    free(fs->async_blocked);
    free(fs->async_enqueued);
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

    /* P2: use cached EOF offsets (updated after each successful write).
     * Skip seek_end+ftello — those cost ~2 syscalls each, dominating the
     * hot path. The cached value is correct as long as we're the only
     * writer; fs_refresh_next_seq resets it when needed (e.g. read that
     * leaves the stream mid-file). */
    if (fs->data_end_off == UINT64_MAX || fs->idx_end_off == UINT64_MAX) {
        /* Cold-start cache: one-time seek+ftell. */
        if (fs_seek_end(fs->data_fp) != 0 || fs_seek_end(fs->idx_fp) != 0) {
            clearerr(fs->idx_fp);
            clearerr(fs->data_fp);
            fs_unlock_pair(fs);
            cmq_mutex_unlock(&fs->lock);
            return -1;
        }
        if (fs_tell(fs->data_fp, &fs->data_end_off) != 0 ||
            fs_tell(fs->idx_fp, &fs->idx_end_off) != 0) {
            clearerr(fs->idx_fp);
            clearerr(fs->data_fp);
            fs_unlock_pair(fs);
            cmq_mutex_unlock(&fs->lock);
            return -1;
        }
    }
    uint64_t offset = fs->data_end_off;
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

    /* P2: advance cached offsets. hdr+payload bytes for data, +8 for idx. */
    fs->data_end_off += (uint64_t)CMQ_FS_HDR_SIZE + (uint64_t)len;
    fs->idx_end_off += 8u;

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

    /* P2: invalidate cached EOF offsets — the read below moves the
     * idx_fp position mid-file. Next append must reseek. */
    fs->data_end_off = UINT64_MAX;
    fs->idx_end_off = UINT64_MAX;

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

static int filestore_maybe_fsync(cmq_filestore_t *fs);

int cmq_filestore_append(cmq_filestore_t *fs, const uint8_t *data, size_t len,
                          uint64_t *out_seq) {
    if (!fs || !data || len == 0) return -1;
    if (fs_begin_op(fs) != 0) return -1;
    int rc = filestore_append_impl(fs, data, len, out_seq);
    if (rc == 0 && fs->fsync_interval_ms > 0) {
        if (filestore_maybe_fsync(fs) != 0) {
            /* fsync failure is not fatal for the append itself;
             * the data is on disk via fflush, just not durable across
             * a crash. */
        }
    }
    fs_end_op(fs);
    return rc;
}

int cmq_filestore_async_enqueue(cmq_filestore_t *fs, const uint8_t *data,
                                 size_t len, uint64_t seq) {
    if (!fs || !fs->async_active || !data || len == 0) return -1;
    pthread_mutex_lock(&fs->async_lock);
    /* P1 v0.5.5: OOM guard. Default 1 MiB. */
    if (fs->max_payload_bytes > 0 && len > fs->max_payload_bytes) {
        pthread_mutex_unlock(&fs->async_lock);
        if (fs->async_blocked)
            cmq_atomic_fetch_add_u64(fs->async_blocked, 1,
                                      CMQ_ATOMIC_RELAXED);
        return -1;
    }
    /* P2 (v0.5.3): bounded wait. Without this a slow worker stalls
     * publishers indefinitely. We use a 10s timeout — long enough
     * for normal bursts, short enough that operators can detect
     * a stuck worker via stat_async_blocked. */
    int blocked = 0;
    while ((unsigned)fs->async_count >= fs->async_capacity &&
           !atomic_load(&fs->dying)) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 10;
        int rc = pthread_cond_timedwait(&fs->async_not_full,
                                          &fs->async_lock, &deadline);
        if (rc == ETIMEDOUT) {
            blocked = 1;
            break;
        }
    }
    if (blocked) {
        pthread_mutex_unlock(&fs->async_lock);
        if (fs->async_blocked)
            cmq_atomic_fetch_add_u64(fs->async_blocked, 1,
                                      CMQ_ATOMIC_RELAXED);
        return -1;
    }
    if (atomic_load(&fs->dying)) {
        pthread_mutex_unlock(&fs->async_lock);
        return -1;
    }
    /* P1 v0.5.5: allocate BEFORE advancing the head pointer. If
     * malloc fails, return -1 without consuming a slot — otherwise
     * a permanent failure would silently leak ring entries. */
    uint8_t *copy = malloc(len > 0 ? len : 1);
    if (!copy) {
        pthread_mutex_unlock(&fs->async_lock);
        return -1;
    }
    memcpy(copy, data, len);
    fs->async_entries[fs->async_head] = copy;
    fs->async_lens[fs->async_head] = len;
    fs->async_seqs[fs->async_head] = seq;
    fs->async_head = (fs->async_head + 1) % fs->async_capacity;
    fs->async_count++;
    pthread_cond_signal(&fs->async_not_empty);
    pthread_mutex_unlock(&fs->async_lock);
    if (fs->async_enqueued)
        cmq_atomic_fetch_add_u64(fs->async_enqueued, 1,
                                  CMQ_ATOMIC_RELAXED);
    return 0;
}

/* P3: install a periodic fsync policy. interval_ms=0 disables
 * periodic fsync (default; only explicit cmq_filestore_sync forces
 * durability). interval_ms>0 calls fdatasync on the data fd every
 * interval_ms milliseconds. */
void cmq_filestore_set_sync_interval(cmq_filestore_t *fs,
                                       unsigned interval_ms) {
    if (!fs) return;
    fs->fsync_interval_ms = interval_ms;
}

void cmq_filestore_set_max_payload_size(cmq_filestore_t *fs, size_t bytes) {
    if (!fs) return;
    fs->max_payload_bytes = bytes;
}

static int filestore_maybe_fsync(cmq_filestore_t *fs) {
    if (!fs->fsync_interval_ms) return 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ULL +
                      (uint64_t)ts.tv_nsec / 1000000ULL;
    if (fs->last_sync_ms == 0 ||
        now_ms - fs->last_sync_ms >= fs->fsync_interval_ms) {
        if (fs->data_fp && fflush(fs->data_fp) != 0) return -1;
        if (fs->idx_fp && fflush(fs->idx_fp) != 0) return -1;
        if (fs->data_fp && fdatasync(fileno(fs->data_fp)) != 0) return -1;
        fs->last_sync_ms = now_ms;
    }
    return 0;
}

int cmq_filestore_read(cmq_filestore_t *fs, uint64_t seq,
                          uint8_t **out_data, size_t *out_len) {
    int rc = fs_begin_op(fs);
    if (rc != 0) return -1;
    rc = filestore_read_impl(fs, seq, out_data, out_len);
    fs_end_op(fs);
    return rc;
}

/* P7: read a contiguous range of records in one pass. The output array
 * is allocated; caller frees with cmq_filestore_range_free. Skips records
 * whose seq is out of range (e.g. gaps). Returns count read, or -1. */
static int filestore_read_range_impl(cmq_filestore_t *fs, uint64_t seq_lo,
                                      uint64_t seq_hi,
                                      cmq_filestore_range_entry_t **out_arr,
                                      size_t *out_count) {
    if (!fs || !out_arr || !out_count || seq_lo == 0 || seq_hi < seq_lo)
        return -1;
    cmq_mutex_lock(&fs->lock);
    if (!fs->data_fp || !fs->idx_fp) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    if (fs_lock_pair(fs, LOCK_SH) != 0) {
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    uint64_t next_seq = 0;
    if (fs_refresh_next_seq(fs, 0) != 0) {
        fs_unlock_pair(fs);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    next_seq = fs->next_seq;
    /* P2: invalidate cache (read moves stream position). */
    fs->data_end_off = UINT64_MAX;
    fs->idx_end_off = UINT64_MAX;

    if (seq_hi >= next_seq) seq_hi = next_seq - 1;
    uint64_t n = seq_hi - seq_lo + 1;
    if (n > 65536u) n = 65536u;

    cmq_filestore_range_entry_t *arr = calloc((size_t)n, sizeof(*arr));
    if (!arr) {
        fs_unlock_pair(fs);
        cmq_mutex_unlock(&fs->lock);
        return -1;
    }
    size_t got = 0;
    /* Bulk-read the entire index range in one fread (saves 1 syscall/seq). */
    uint64_t idx_off = (seq_lo - 1) * 8u;
    if (fs_seek(fs->idx_fp, (long)idx_off) != 0) goto out;
    uint8_t *idx_buf = malloc((size_t)n * 8u);
    if (!idx_buf) goto out;
    if (fread(idx_buf, 8u, (size_t)n, fs->idx_fp) != (size_t)n) {
        free(idx_buf);
        goto out;
    }
    /* Now read each data record. Each record has its own offset. */
    for (uint64_t i = 0; i < n; i++) {
        uint64_t data_offset = get_le64(idx_buf + i * 8u);
        uint8_t hdr[CMQ_FS_HDR_SIZE];
        if (fs_seek(fs->data_fp, (long)data_offset) != 0) continue;
        if (fread(hdr, sizeof(hdr), 1, fs->data_fp) != 1) continue;
        if (get_le32(hdr + 0) != CMQ_FS_MAGIC) continue;
        uint32_t plen = get_le32(hdr + 14);
        if (plen == 0 || plen > 16u * 1024 * 1024) continue;
        uint8_t *payload = malloc(plen);
        if (!payload) continue;
        if (fread(payload, 1, plen, fs->data_fp) != plen) {
            free(payload);
            continue;
        }
        arr[got].data = payload;
        arr[got].len = plen;
        got++;
    }
    free(idx_buf);

out:
    fs_unlock_pair(fs);
    cmq_mutex_unlock(&fs->lock);
    *out_arr = arr;
    *out_count = got;
    return (int)got;
}

int cmq_filestore_read_range(cmq_filestore_t *fs, uint64_t seq_lo,
                              uint64_t seq_hi,
                              cmq_filestore_range_entry_t **out_arr,
                              size_t *out_count) {
    int rc = fs_begin_op(fs);
    if (rc != 0) return -1;
    rc = filestore_read_range_impl(fs, seq_lo, seq_hi, out_arr, out_count);
    fs_end_op(fs);
    return rc;
}

void cmq_filestore_range_free(cmq_filestore_range_entry_t *arr, size_t n) {
    if (!arr) return;
    for (size_t i = 0; i < n; i++) {
        free(arr[i].data);
    }
    free(arr);
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

uint64_t cmq_filestore_async_blocked_count(cmq_filestore_t *fs) {
    if (!fs || !fs->async_blocked) return 0;
    return cmq_atomic_load_u64(fs->async_blocked, CMQ_ATOMIC_RELAXED);
}

uint64_t cmq_filestore_async_enqueued_count(cmq_filestore_t *fs) {
    if (!fs || !fs->async_enqueued) return 0;
    return cmq_atomic_load_u64(fs->async_enqueued, CMQ_ATOMIC_RELAXED);
}

static void *async_worker(void *arg) {
    cmq_filestore_t *fs = (cmq_filestore_t *)arg;
    while (!atomic_load(&fs->dying)) {
        pthread_mutex_lock(&fs->async_lock);
        while (fs->async_count == 0 && !atomic_load(&fs->dying)) {
            pthread_cond_wait(&fs->async_not_empty, &fs->async_lock);
        }
        if (atomic_load(&fs->dying) && fs->async_count == 0) {
            pthread_mutex_unlock(&fs->async_lock);
            break;
        }
        uint8_t *entry = fs->async_entries[fs->async_tail];
        size_t len = fs->async_lens[fs->async_tail];
        fs->async_tail = (fs->async_tail + 1) % fs->async_capacity;
        fs->async_count--;
        pthread_cond_signal(&fs->async_not_full);
        pthread_mutex_unlock(&fs->async_lock);

        cmq_mutex_lock(&fs->lock);
        if (fs->data_fp && fs->idx_fp) {
            (void)fwrite(entry, 1, len, fs->data_fp);
            (void)fflush(fs->data_fp);
            (void)fflush(fs->idx_fp);
        }
        cmq_mutex_unlock(&fs->lock);
        free(entry);
    }
    return NULL;
}

int cmq_filestore_set_async(cmq_filestore_t *fs, unsigned queue_capacity) {
    if (!fs) return -1;
    if (fs->async_active) return 0;
    if (queue_capacity == 0) return -1;
    fs->async_capacity = queue_capacity;
    fs->async_head = fs->async_tail = fs->async_count = 0;
    fs->async_entries = calloc(queue_capacity, sizeof(uint8_t *));
    fs->async_lens = calloc(queue_capacity, sizeof(size_t));
    fs->async_seqs = calloc(queue_capacity, sizeof(uint64_t));
    if (!fs->async_entries || !fs->async_lens || !fs->async_seqs) {
        free(fs->async_entries);
        free(fs->async_lens);
        free(fs->async_seqs);
        return -1;
    }
    pthread_mutex_init(&fs->async_lock, NULL);
    pthread_cond_init(&fs->async_not_empty, NULL);
    pthread_cond_init(&fs->async_not_full, NULL);
    if (pthread_create(&fs->async_thread, NULL, async_worker, fs) != 0) {
        pthread_mutex_destroy(&fs->async_lock);
        pthread_cond_destroy(&fs->async_not_empty);
        pthread_cond_destroy(&fs->async_not_full);
        free(fs->async_entries);
        free(fs->async_lens);
        free(fs->async_seqs);
        return -1;
    }
    fs->async_active = 1;
    return 0;
}
