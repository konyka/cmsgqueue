#define _POSIX_C_SOURCE 200809L
#include "cmq_leaf.h"
#include "cmq_route.h"
#include "cmq_parser.h"
#include "cmq_proto.h"
#include "cmq_thread.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

#define CMQ_LEAF_MAX_SUBS 1024
#define CMQ_LEAF_WRITE_MS 50
#define CMQ_LEAF_CONNECT_MS 2000

struct cmq_leaf_node {
    char hub_addr[CMQ_NODE_ADDR_SIZE];
    int hub_port;
    int hub_fd;
    int connected;
    char auth_user[256];
    char auth_pass[256];

    char *subs[CMQ_LEAF_MAX_SUBS];
    uint32_t sub_ids[CMQ_LEAF_MAX_SUBS];
    size_t sub_count;
    uint32_t next_sub_id;

    /* Offline UNSUBs to flush on next connect (before SUB replay). */
    uint32_t pending_unsub[CMQ_LEAF_MAX_SUBS];
    size_t pending_unsub_count;

    cmq_leaf_conn_t leaves[CMQ_LEAF_MAX_CONNECTIONS];
    size_t leaf_count;

    cmq_mutex_t lock;
    cmq_mutex_t hub_io_lock; /* serialize hub writes vs disconnect close */
};

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Drop dead hub TCP under hub_io_lock. Keeps local interest for reconnect replay.
   expect_fd < 0 clears any hub; otherwise only if hub_fd still matches. */
static void leaf_hub_drop(cmq_leaf_node_t *leaf, int expect_fd) {
    cmq_mutex_lock(&leaf->lock);
    if (expect_fd >= 0 && leaf->hub_fd != expect_fd) {
        cmq_mutex_unlock(&leaf->lock);
        return;
    }
    int fd = leaf->hub_fd;
    leaf->hub_fd = -1;
    leaf->connected = 0;
    cmq_mutex_unlock(&leaf->lock);
    if (fd >= 0) close(fd);
}

static int leaf_fd_alive(int fd) {
    if (fd < 0) return 0;
    struct pollfd pfd = { .fd = fd, .events = 0 };
    int pr = poll(&pfd, 1, 0);
    return pr >= 0 && !(pfd.revents & (POLLERR | POLLHUP | POLLNVAL));
}

/* Caller holds hub_io_lock. Re-probe after identity match so a recycled
   live hub_fd is not closed (connect installs under the same lock). */
static void leaf_hub_drop_if_dead(cmq_leaf_node_t *leaf, int expect_fd) {
    cmq_mutex_lock(&leaf->lock);
    int same = (expect_fd >= 0 && leaf->hub_fd == expect_fd);
    cmq_mutex_unlock(&leaf->lock);
    if (same && !leaf_fd_alive(expect_fd))
        leaf_hub_drop(leaf, expect_fd);
}

#define CMQ_LEAF_SUBACK_MS 2000

/* Complete write on nonblocking hub fd (poll on EAGAIN). */
static int write_all(int fd, const uint8_t *data, size_t len) {
    size_t off = 0;
    int stall_rounds = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                for (;;) {
                    int pr = poll(&pfd, 1, CMQ_LEAF_WRITE_MS);
                    if (pr > 0) {
                        stall_rounds = 0;
                        break;
                    }
                    if (pr < 0 && errno == EINTR) continue;
                    if (pr == 0 && ++stall_rounds < 4)
                        continue;
                    return -1;
                }
                continue;
            }
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/* Discard `nbyte` from fd (after partial buffer already dropped). */
static int leaf_discard_bytes(int fd, size_t nbyte, int *waited_ms) {
    uint8_t junk[1024];
    while (nbyte > 0) {
        if (*waited_ms >= CMQ_LEAF_SUBACK_MS) return -1;
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) {
            *waited_ms += 100;
            continue;
        }
        size_t chunk = nbyte > sizeof(junk) ? sizeof(junk) : nbyte;
        ssize_t n = read(fd, junk, chunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        nbyte -= (size_t)n;
    }
    return 0;
}

/* Wait for SUBACK(sub_id) with code 0. Skips INFO/PONG; drains MESSAGE
   (leaf API is interest-only — no RX consumer). Oversized frames are
   stream-discarded so a large MESSAGE cannot abort SUBACK wait. */
static int read_suback(int fd, uint32_t expect_id) {
    uint8_t rbuf[4096];
    size_t rlen = 0;
    int waited_ms = 0;
    while (waited_ms < CMQ_LEAF_SUBACK_MS) {
        while (rlen >= CMQ_PROTO_HDR_SIZE) {
            if (rbuf[0] != CMQ_PROTO_MAGIC_0 || rbuf[1] != CMQ_PROTO_MAGIC_1)
                return -1;
            if (rbuf[2] != CMQ_PROTO_VERSION)
                return -1;
            uint8_t op = rbuf[4];
            uint32_t plen = (uint32_t)rbuf[5] | ((uint32_t)rbuf[6] << 8) |
                            ((uint32_t)rbuf[7] << 16) | ((uint32_t)rbuf[8] << 24);
            size_t need = (size_t)CMQ_PROTO_HDR_SIZE + (size_t)plen;
            if (need > sizeof(rbuf)) {
                /* Drop buffered prefix; discard the rest from the socket. */
                size_t have = rlen;
                rlen = 0;
                if (have > need) return -1;
                if (leaf_discard_bytes(fd, need - have, &waited_ms) != 0)
                    return -1;
                if (op == (uint8_t)CMQ_OP_ERROR ||
                    op == (uint8_t)CMQ_OP_DISCONNECT)
                    return -1;
                continue;
            }
            if (rlen < need) break;

            const uint8_t *pay = rbuf + CMQ_PROTO_HDR_SIZE;
            if (op == (uint8_t)CMQ_OP_SUBACK) {
                if (plen < 5) return -1;
                uint8_t code = pay[0];
                uint32_t sid = ((uint32_t)pay[1] << 24) | ((uint32_t)pay[2] << 16) |
                               ((uint32_t)pay[3] << 8) | (uint32_t)pay[4];
                memmove(rbuf, rbuf + need, rlen - need);
                rlen -= need;
                if (sid != expect_id) continue;
                return code == 0 ? 0 : -1;
            }
            if (op == (uint8_t)CMQ_OP_ERROR || op == (uint8_t)CMQ_OP_DISCONNECT)
                return -1;
            /* INFO/PONG/MESSAGE/UNSUBACK — consume and keep waiting. */
            memmove(rbuf, rbuf + need, rlen - need);
            rlen -= need;
        }

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) {
            waited_ms += 100;
            continue;
        }
        if (rlen >= sizeof(rbuf)) return -1;
        ssize_t n = read(fd, rbuf + rlen, sizeof(rbuf) - rlen);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        rlen += (size_t)n;
    }
    return -1;
}

cmq_leaf_node_t *cmq_leaf_create(const char *hub_addr, int hub_port) {
    if (!hub_addr) return NULL;
    cmq_leaf_node_t *l = calloc(1, sizeof(cmq_leaf_node_t));
    if (!l) return NULL;
    strncpy(l->hub_addr, hub_addr, CMQ_NODE_ADDR_SIZE - 1);
    l->hub_port = hub_port;
    l->hub_fd = -1;
    l->connected = 0;
    l->next_sub_id = 1; /* server rejects sub_id 0 */
    l->sub_count = 0;
    l->leaf_count = 0;
    cmq_mutex_init(&l->lock);
    cmq_mutex_init(&l->hub_io_lock);
    return l;
}

void cmq_leaf_destroy(cmq_leaf_node_t *leaf) {
    if (!leaf) return;
    cmq_mutex_lock(&leaf->hub_io_lock);
    cmq_mutex_lock(&leaf->lock);
    if (leaf->hub_fd >= 0) close(leaf->hub_fd);
    leaf->hub_fd = -1;
    leaf->connected = 0;
    for (size_t i = 0; i < leaf->leaf_count; i++) {
        if (leaf->leaves[i].fd >= 0) close(leaf->leaves[i].fd);
        leaf->leaves[i].fd = -1;
    }
    leaf->leaf_count = 0;
    for (size_t i = 0; i < leaf->sub_count; i++) {
        free(leaf->subs[i]);
        leaf->subs[i] = NULL;
    }
    leaf->sub_count = 0;
    cmq_mutex_unlock(&leaf->lock);
    cmq_mutex_unlock(&leaf->hub_io_lock);
    cmq_mutex_destroy(&leaf->hub_io_lock);
    cmq_mutex_destroy(&leaf->lock);
    free(leaf);
}

int cmq_leaf_set_auth(cmq_leaf_node_t *leaf, const char *user, const char *pass) {
    if (!leaf) return -1;
    cmq_mutex_lock(&leaf->lock);
    memset(leaf->auth_user, 0, sizeof(leaf->auth_user));
    memset(leaf->auth_pass, 0, sizeof(leaf->auth_pass));
    if (user && user[0])
        strncpy(leaf->auth_user, user, sizeof(leaf->auth_user) - 1);
    if (pass && pass[0])
        strncpy(leaf->auth_pass, pass, sizeof(leaf->auth_pass) - 1);
    cmq_mutex_unlock(&leaf->lock);
    return 0;
}

static int leaf_handshake(cmq_leaf_node_t *leaf, int fd) {
    char user[256], pass[256];
    cmq_mutex_lock(&leaf->lock);
    strncpy(user, leaf->auth_user, sizeof(user) - 1);
    user[sizeof(user) - 1] = '\0';
    strncpy(pass, leaf->auth_pass, sizeof(pass) - 1);
    pass[sizeof(pass) - 1] = '\0';
    cmq_mutex_unlock(&leaf->lock);
    return cmq_peer_handshake(fd, user[0] ? user : NULL, pass[0] ? pass : NULL, 0);
}

const char *cmq_leaf_hub_addr(cmq_leaf_node_t *leaf) {
    return leaf ? leaf->hub_addr : NULL;
}

int cmq_leaf_hub_port(cmq_leaf_node_t *leaf) {
    return leaf ? leaf->hub_port : 0;
}

int cmq_leaf_connect(cmq_leaf_node_t *leaf) {
    if (!leaf) return -1;
    cmq_mutex_lock(&leaf->lock);

    if (leaf->connected && leaf->hub_fd >= 0) {
        int fd = leaf->hub_fd;
        cmq_mutex_unlock(&leaf->lock);
        int alive = leaf_fd_alive(fd);
        cmq_mutex_lock(&leaf->lock);
        if (alive && leaf->connected && leaf->hub_fd == fd) {
            cmq_mutex_unlock(&leaf->lock);
            return 0;
        }
        cmq_mutex_unlock(&leaf->lock);
        cmq_mutex_lock(&leaf->hub_io_lock);
        leaf_hub_drop_if_dead(leaf, fd);
        cmq_mutex_unlock(&leaf->hub_io_lock);
        cmq_mutex_lock(&leaf->lock);
        if (leaf->connected && leaf->hub_fd >= 0) {
            int nfd = leaf->hub_fd;
            cmq_mutex_unlock(&leaf->lock);
            alive = leaf_fd_alive(nfd);
            cmq_mutex_lock(&leaf->lock);
            if (alive && leaf->connected && leaf->hub_fd == nfd) {
                cmq_mutex_unlock(&leaf->lock);
                return 0;
            }
            cmq_mutex_unlock(&leaf->lock);
            cmq_mutex_lock(&leaf->hub_io_lock);
            leaf_hub_drop_if_dead(leaf, nfd);
            cmq_mutex_unlock(&leaf->hub_io_lock);
            cmq_mutex_lock(&leaf->lock);
        }
    }

    char addr_copy[CMQ_NODE_ADDR_SIZE];
    strncpy(addr_copy, leaf->hub_addr, sizeof(addr_copy) - 1);
    addr_copy[sizeof(addr_copy) - 1] = '\0';
    int port_copy = leaf->hub_port;
    cmq_mutex_unlock(&leaf->lock);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port_copy);
    if (inet_pton(AF_INET, addr_copy, &sa.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (cmq_connect_timeout(fd, (struct sockaddr *)&sa, sizeof(sa),
                             CMQ_LEAF_CONNECT_MS) != 0) {
        close(fd);
        return -1;
    }
    if (leaf_handshake(leaf, fd) != 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);

    /* Publish hub_fd under hub_io_lock so disconnect cannot close mid-replay. */
    cmq_mutex_lock(&leaf->hub_io_lock);
    cmq_mutex_lock(&leaf->lock);
    if (leaf->connected && leaf->hub_fd >= 0) {
        int efd = leaf->hub_fd;
        cmq_mutex_unlock(&leaf->lock);
        cmq_mutex_unlock(&leaf->hub_io_lock);
        int alive = leaf_fd_alive(efd);
        cmq_mutex_lock(&leaf->hub_io_lock);
        cmq_mutex_lock(&leaf->lock);
        if (alive && leaf->connected && leaf->hub_fd == efd) {
            cmq_mutex_unlock(&leaf->lock);
            cmq_mutex_unlock(&leaf->hub_io_lock);
            close(fd);
            return 0;
        }
        cmq_mutex_unlock(&leaf->lock);
        leaf_hub_drop_if_dead(leaf, efd);
        cmq_mutex_lock(&leaf->lock);
        if (leaf->connected && leaf->hub_fd >= 0) {
            int nfd = leaf->hub_fd;
            cmq_mutex_unlock(&leaf->lock);
            cmq_mutex_unlock(&leaf->hub_io_lock);
            alive = leaf_fd_alive(nfd);
            cmq_mutex_lock(&leaf->hub_io_lock);
            cmq_mutex_lock(&leaf->lock);
            if (alive && leaf->connected && leaf->hub_fd == nfd) {
                cmq_mutex_unlock(&leaf->lock);
                cmq_mutex_unlock(&leaf->hub_io_lock);
                close(fd);
                return 0;
            }
            cmq_mutex_unlock(&leaf->lock);
            leaf_hub_drop_if_dead(leaf, nfd);
            cmq_mutex_lock(&leaf->lock);
        }
    }
    leaf->hub_fd = fd;
    /* connected=1 only after hub SUB replay — is_connected must not race. */
    leaf->connected = 0;
    /* Flush offline UNSUBs before replaying live interest. */
    size_t pn = leaf->pending_unsub_count;
    uint32_t *pending = NULL;
    if (pn > 0) {
        pending = malloc(pn * sizeof(uint32_t));
        if (!pending) {
            leaf->hub_fd = -1;
            leaf->connected = 0;
            cmq_mutex_unlock(&leaf->lock);
            cmq_mutex_unlock(&leaf->hub_io_lock);
            close(fd);
            return -1;
        }
        memcpy(pending, leaf->pending_unsub, pn * sizeof(uint32_t));
        leaf->pending_unsub_count = 0;
    }
    /* Keep next_sub_id; replay existing interest to the new hub. */
    size_t n = leaf->sub_count;
    char **subjects = NULL;
    uint32_t *ids = NULL;
    if (n > 0) {
        subjects = malloc(n * sizeof(char *));
        ids = malloc(n * sizeof(uint32_t));
        if (!subjects || !ids) {
            free(subjects);
            free(ids);
            if (pending) {
                /* Restore pending so a later connect can retry. */
                size_t restore = pn;
                if (restore > CMQ_LEAF_MAX_SUBS - leaf->pending_unsub_count)
                    restore = CMQ_LEAF_MAX_SUBS - leaf->pending_unsub_count;
                memcpy(leaf->pending_unsub + leaf->pending_unsub_count, pending,
                       restore * sizeof(uint32_t));
                leaf->pending_unsub_count += restore;
                free(pending);
            }
            leaf->hub_fd = -1;
            leaf->connected = 0;
            cmq_mutex_unlock(&leaf->lock);
            cmq_mutex_unlock(&leaf->hub_io_lock);
            close(fd);
            return -1;
        }
        for (size_t i = 0; i < n; i++) {
            /* Deep copy — unsubscribe may free leaf->subs[i] after unlock. */
            subjects[i] = strdup(leaf->subs[i]);
            if (!subjects[i]) {
                for (size_t j = 0; j < i; j++) free(subjects[j]);
                free(subjects);
                free(ids);
                if (pending) {
                    size_t restore = pn;
                    if (restore > CMQ_LEAF_MAX_SUBS - leaf->pending_unsub_count)
                        restore = CMQ_LEAF_MAX_SUBS - leaf->pending_unsub_count;
                    memcpy(leaf->pending_unsub + leaf->pending_unsub_count,
                           pending, restore * sizeof(uint32_t));
                    leaf->pending_unsub_count += restore;
                    free(pending);
                }
                leaf->hub_fd = -1;
                leaf->connected = 0;
                cmq_mutex_unlock(&leaf->lock);
                cmq_mutex_unlock(&leaf->hub_io_lock);
                close(fd);
                return -1;
            }
            ids[i] = leaf->sub_ids[i];
        }
    }
    cmq_mutex_unlock(&leaf->lock);

    for (size_t i = 0; i < pn; i++) {
        uint32_t uid = pending[i];
        uint8_t upay[4] = {
            (uint8_t)(uid >> 24), (uint8_t)(uid >> 16),
            (uint8_t)(uid >> 8), (uint8_t)uid
        };
        uint8_t uframe[16];
        size_t ulen = cmq_frame_encode(uframe, sizeof(uframe),
                                        CMQ_OP_UNSUBSCRIBE, 0, upay, 4);
        if (ulen == 0 || write_all(fd, uframe, ulen) != 0) {
            cmq_mutex_lock(&leaf->lock);
            /* Restore remaining pending UNSUBs for a later connect. */
            for (size_t r = i; r < pn; r++) {
                if (leaf->pending_unsub_count >= CMQ_LEAF_MAX_SUBS)
                    break;
                leaf->pending_unsub[leaf->pending_unsub_count++] = pending[r];
            }
            if (leaf->hub_fd == fd) {
                leaf->hub_fd = -1;
                leaf->connected = 0;
            }
            cmq_mutex_unlock(&leaf->lock);
            for (size_t j = 0; j < n; j++) free(subjects[j]);
            free(subjects);
            free(ids);
            free(pending);
            cmq_mutex_unlock(&leaf->hub_io_lock);
            close(fd);
            return -1;
        }
    }
    free(pending);

    for (size_t i = 0; i < n; i++) {
        const char *subject = subjects[i];
        size_t slen = strlen(subject);
        uint32_t sub_id = ids[i];
        uint8_t payload[8 + 256];
        size_t po = 0;
        payload[po++] = (uint8_t)(sub_id >> 24);
        payload[po++] = (uint8_t)(sub_id >> 16);
        payload[po++] = (uint8_t)(sub_id >> 8);
        payload[po++] = (uint8_t)sub_id;
        payload[po++] = (uint8_t)(slen >> 8);
        payload[po++] = (uint8_t)slen;
        memcpy(payload + po, subject, slen);
        po += slen;
        uint8_t frame[16 + 256];
        size_t flen = cmq_frame_encode(frame, sizeof(frame), CMQ_OP_SUBSCRIBE,
                                        0, payload, po);
        if (flen == 0 || write_all(fd, frame, flen) != 0 ||
            read_suback(fd, sub_id) != 0) {
            /* Best-effort UNSUB for subjects already pushed this reconnect so
               hub does not keep interest that leaf will re-play on next connect. */
            for (size_t u = 0; u < i; u++) {
                uint32_t uid = ids[u];
                uint8_t upay[4] = {
                    (uint8_t)(uid >> 24), (uint8_t)(uid >> 16),
                    (uint8_t)(uid >> 8), (uint8_t)uid
                };
                uint8_t uframe[16];
                size_t ulen = cmq_frame_encode(uframe, sizeof(uframe),
                                                CMQ_OP_UNSUBSCRIBE, 0, upay, 4);
                if (ulen > 0)
                    (void)write_all(fd, uframe, ulen);
            }
            for (size_t j = 0; j < n; j++) free(subjects[j]);
            free(subjects);
            free(ids);
            cmq_mutex_lock(&leaf->lock);
            if (leaf->hub_fd == fd) {
                leaf->hub_fd = -1;
                leaf->connected = 0;
            }
            cmq_mutex_unlock(&leaf->lock);
            cmq_mutex_unlock(&leaf->hub_io_lock);
            close(fd);
            return -1;
        }
    }
    cmq_mutex_lock(&leaf->lock);
    if (leaf->hub_fd == fd)
        leaf->connected = 1;
    cmq_mutex_unlock(&leaf->lock);
    cmq_mutex_unlock(&leaf->hub_io_lock);
    for (size_t j = 0; j < n; j++) free(subjects[j]);
    free(subjects);
    free(ids);
    return 0;
}

int cmq_leaf_disconnect(cmq_leaf_node_t *leaf) {
    if (!leaf) return -1;
    cmq_mutex_lock(&leaf->hub_io_lock);
    cmq_mutex_lock(&leaf->lock);
    if (leaf->hub_fd >= 0) close(leaf->hub_fd);
    leaf->hub_fd = -1;
    leaf->connected = 0;
    cmq_mutex_unlock(&leaf->lock);
    cmq_mutex_unlock(&leaf->hub_io_lock);
    return 0;
}

int cmq_leaf_is_connected(cmq_leaf_node_t *leaf) {
    if (!leaf) return 0;
    cmq_mutex_lock(&leaf->lock);
    int fd = leaf->hub_fd;
    int c = leaf->connected && fd >= 0;
    cmq_mutex_unlock(&leaf->lock);
    if (!c) return 0;
    /* Same light probe as connect — clear sticky connected on dead hub. */
    int alive = leaf_fd_alive(fd);
    cmq_mutex_lock(&leaf->lock);
    if (alive && leaf->connected && leaf->hub_fd == fd) {
        cmq_mutex_unlock(&leaf->lock);
        return 1;
    }
    cmq_mutex_unlock(&leaf->lock);
    cmq_mutex_lock(&leaf->hub_io_lock);
    leaf_hub_drop_if_dead(leaf, fd);
    cmq_mutex_unlock(&leaf->hub_io_lock);
    return 0;
}

int cmq_leaf_subscribe(cmq_leaf_node_t *leaf, const char *subject) {
    if (!leaf || !subject) return -1;
    size_t slen = strlen(subject);
    if (slen == 0 || slen >= 256) return -1;

    char *copy = strdup(subject);
    if (!copy) return -1;

    /* hub_io → leaf: claim serialized with unsubscribe drop. */
    cmq_mutex_lock(&leaf->hub_io_lock);
    cmq_mutex_lock(&leaf->lock);
    if (leaf->sub_count >= CMQ_LEAF_MAX_SUBS) {
        cmq_mutex_unlock(&leaf->lock);
        cmq_mutex_unlock(&leaf->hub_io_lock);
        free(copy);
        return -1;
    }
    for (size_t i = 0; i < leaf->sub_count; i++) {
        if (strcmp(leaf->subs[i], subject) == 0) {
            cmq_mutex_unlock(&leaf->lock);
            cmq_mutex_unlock(&leaf->hub_io_lock);
            free(copy);
            return 0;
        }
    }
    if (leaf->next_sub_id == 0)
        leaf->next_sub_id = 1;
    uint32_t sub_id = leaf->next_sub_id++;
    if (leaf->next_sub_id == 0)
        leaf->next_sub_id = 1;
    size_t idx = leaf->sub_count;
    leaf->subs[idx] = copy;
    leaf->sub_ids[idx] = sub_id;
    leaf->sub_count++;
    int hub_fd = leaf->hub_fd;
    int connected = leaf->connected;
    cmq_mutex_unlock(&leaf->lock);

    if (!(connected && hub_fd >= 0)) {
        cmq_mutex_unlock(&leaf->hub_io_lock);
        return 0;
    }

    uint8_t payload[8 + 256];
    size_t po = 0;
    payload[po++] = (uint8_t)(sub_id >> 24);
    payload[po++] = (uint8_t)(sub_id >> 16);
    payload[po++] = (uint8_t)(sub_id >> 8);
    payload[po++] = (uint8_t)sub_id;
    payload[po++] = (uint8_t)(slen >> 8);
    payload[po++] = (uint8_t)slen;
    memcpy(payload + po, subject, slen);
    po += slen;
    uint8_t frame[16 + 256];
    size_t flen = cmq_frame_encode(frame, sizeof(frame), CMQ_OP_SUBSCRIBE,
                                    0, payload, po);
    /* Hub cannot switch while we hold hub_io (connect takes same lock). */
    int wr = (flen > 0) ? write_all(hub_fd, frame, flen) : -1;
    int ack_ok = (wr == 0 && flen > 0) ? read_suback(hub_fd, sub_id) : -1;
    if (flen == 0 || wr != 0 || ack_ok != 0) {
        if (wr != 0 || flen == 0)
            leaf_hub_drop(leaf, hub_fd);
        else {
            /* Roll back hub interest; drop hub if UNSUB cannot be written. */
            uint8_t upay[4] = {
                (uint8_t)(sub_id >> 24), (uint8_t)(sub_id >> 16),
                (uint8_t)(sub_id >> 8), (uint8_t)sub_id
            };
            uint8_t uframe[16];
            size_t ulen = cmq_frame_encode(uframe, sizeof(uframe),
                                            CMQ_OP_UNSUBSCRIBE, 0, upay, 4);
            if (ulen == 0 || write_all(hub_fd, uframe, ulen) != 0)
                leaf_hub_drop(leaf, hub_fd);
        }
        cmq_mutex_lock(&leaf->lock);
        for (size_t i = 0; i < leaf->sub_count; i++) {
            if (leaf->sub_ids[i] == sub_id && leaf->subs[i] &&
                strcmp(leaf->subs[i], subject) == 0) {
                free(leaf->subs[i]);
                memmove(&leaf->subs[i], &leaf->subs[i + 1],
                        (leaf->sub_count - i - 1) * sizeof(char *));
                memmove(&leaf->sub_ids[i], &leaf->sub_ids[i + 1],
                        (leaf->sub_count - i - 1) * sizeof(uint32_t));
                leaf->sub_count--;
                break;
            }
        }
        cmq_mutex_unlock(&leaf->lock);
        cmq_mutex_unlock(&leaf->hub_io_lock);
        return -1;
    }
    cmq_mutex_unlock(&leaf->hub_io_lock);
    return 0;
}

int cmq_leaf_unsubscribe(cmq_leaf_node_t *leaf, const char *subject) {
    if (!leaf || !subject) return -1;

    /* hub_io → leaf: drop serialized with subscribe claim. */
    cmq_mutex_lock(&leaf->hub_io_lock);
    cmq_mutex_lock(&leaf->lock);
    for (size_t i = 0; i < leaf->sub_count; i++) {
        if (strcmp(leaf->subs[i], subject) != 0)
            continue;
        uint32_t sub_id = leaf->sub_ids[i];
        int hub_fd = leaf->hub_fd;
        int connected = leaf->connected;
        if (!(connected && hub_fd >= 0) &&
            leaf->pending_unsub_count >= CMQ_LEAF_MAX_SUBS) {
            cmq_mutex_unlock(&leaf->lock);
            cmq_mutex_unlock(&leaf->hub_io_lock);
            return -1;
        }
        free(leaf->subs[i]);
        memmove(&leaf->subs[i], &leaf->subs[i + 1],
                (leaf->sub_count - i - 1) * sizeof(char *));
        memmove(&leaf->sub_ids[i], &leaf->sub_ids[i + 1],
                (leaf->sub_count - i - 1) * sizeof(uint32_t));
        leaf->sub_count--;
        if (!(connected && hub_fd >= 0)) {
            leaf->pending_unsub[leaf->pending_unsub_count++] = sub_id;
            cmq_mutex_unlock(&leaf->lock);
            cmq_mutex_unlock(&leaf->hub_io_lock);
            return 0;
        }
        cmq_mutex_unlock(&leaf->lock);

        uint8_t payload[4] = {
            (uint8_t)(sub_id >> 24), (uint8_t)(sub_id >> 16),
            (uint8_t)(sub_id >> 8), (uint8_t)sub_id
        };
        uint8_t frame[16];
        size_t flen = cmq_frame_encode(frame, sizeof(frame),
                                        CMQ_OP_UNSUBSCRIBE, 0, payload, 4);
        int wr = (flen > 0) ? write_all(hub_fd, frame, flen) : -1;
        if (flen == 0 || wr != 0)
            leaf_hub_drop(leaf, hub_fd);
        cmq_mutex_unlock(&leaf->hub_io_lock);
        return 0;
    }
    cmq_mutex_unlock(&leaf->lock);
    cmq_mutex_unlock(&leaf->hub_io_lock);
    return -1;
}

size_t cmq_leaf_sub_count(cmq_leaf_node_t *leaf) {
    if (!leaf) return 0;
    cmq_mutex_lock(&leaf->lock);
    size_t c = leaf->sub_count;
    cmq_mutex_unlock(&leaf->lock);
    return c;
}

size_t cmq_leaf_accept_count(cmq_leaf_node_t *leaf) {
    if (!leaf) return 0;
    cmq_mutex_lock(&leaf->lock);
    size_t c = leaf->leaf_count;
    cmq_mutex_unlock(&leaf->lock);
    return c;
}

int cmq_leaf_accept(cmq_leaf_node_t *leaf, int fd, const char *leaf_id) {
    if (!leaf || !leaf_id) return -1;

    cmq_mutex_lock(&leaf->lock);
    if (leaf->leaf_count >= CMQ_LEAF_MAX_CONNECTIONS) {
        /* Allow replace of an existing same leaf_id even when table is full. */
        int have = 0;
        for (size_t i = 0; i < leaf->leaf_count; i++) {
            if (strcmp(leaf->leaves[i].leaf_id, leaf_id) == 0) {
                have = 1;
                break;
            }
        }
        if (!have) {
            cmq_mutex_unlock(&leaf->lock);
            if (fd >= 0) close(fd);
            return -1;
        }
    }
    cmq_mutex_unlock(&leaf->lock);

    /* fd < 0: placeholder slot (tests). Live fd: cluster handshake first. */
    if (fd >= 0) {
        if (leaf_handshake(leaf, fd) != 0) {
            close(fd);
            return -1;
        }
        set_nonblock(fd);
    }

    cmq_mutex_lock(&leaf->lock);
    if (leaf->leaf_count >= CMQ_LEAF_MAX_CONNECTIONS) {
        /* Still allow replace of an existing same leaf_id. */
        int have = 0;
        for (size_t i = 0; i < leaf->leaf_count; i++) {
            if (strcmp(leaf->leaves[i].leaf_id, leaf_id) == 0) {
                have = 1;
                break;
            }
        }
        if (!have) {
            cmq_mutex_unlock(&leaf->lock);
            if (fd >= 0) close(fd);
            return -1;
        }
    }
    for (size_t i = 0; i < leaf->leaf_count; i++) {
        if (strcmp(leaf->leaves[i].leaf_id, leaf_id) == 0) {
            if (leaf->leaves[i].fd >= 0 && leaf->leaves[i].fd != fd)
                close(leaf->leaves[i].fd);
            leaf->leaves[i].fd = fd;
            leaf->leaves[i].connected = 1;
            cmq_mutex_unlock(&leaf->lock);
            return 0;
        }
    }
    if (leaf->leaf_count >= CMQ_LEAF_MAX_CONNECTIONS) {
        cmq_mutex_unlock(&leaf->lock);
        if (fd >= 0) close(fd);
        return -1;
    }
    cmq_leaf_conn_t *c = &leaf->leaves[leaf->leaf_count++];
    strncpy(c->leaf_id, leaf_id, CMQ_NODE_ID_SIZE - 1);
    c->leaf_id[CMQ_NODE_ID_SIZE - 1] = '\0';
    c->fd = fd;
    c->connected = 1;
    c->subscriptions = 0;
    cmq_mutex_unlock(&leaf->lock);
    return 0;
}

int cmq_leaf_remove(cmq_leaf_node_t *leaf, const char *leaf_id) {
    if (!leaf || !leaf_id) return -1;
    cmq_mutex_lock(&leaf->lock);
    for (size_t i = 0; i < leaf->leaf_count; i++) {
        if (strcmp(leaf->leaves[i].leaf_id, leaf_id) == 0) {
            if (leaf->leaves[i].fd >= 0) close(leaf->leaves[i].fd);
            memmove(&leaf->leaves[i], &leaf->leaves[i + 1],
                    (leaf->leaf_count - i - 1) * sizeof(cmq_leaf_conn_t));
            leaf->leaf_count--;
            cmq_mutex_unlock(&leaf->lock);
            return 0;
        }
    }
    cmq_mutex_unlock(&leaf->lock);
    return -1;
}
