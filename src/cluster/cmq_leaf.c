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

    cmq_leaf_conn_t leaves[CMQ_LEAF_MAX_CONNECTIONS];
    size_t leaf_count;

    cmq_mutex_t lock;
    cmq_mutex_t hub_io_lock; /* serialize hub writes vs disconnect close */
};

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

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
    cmq_mutex_unlock(&leaf->lock);
    cmq_mutex_unlock(&leaf->hub_io_lock);
    for (size_t i = 0; i < leaf->leaf_count; i++) {
        if (leaf->leaves[i].fd >= 0) close(leaf->leaves[i].fd);
    }
    for (size_t i = 0; i < leaf->sub_count; i++) {
        free(leaf->subs[i]);
    }
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
    return cmq_peer_handshake(fd, user[0] ? user : NULL, pass[0] ? pass : NULL);
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
        cmq_mutex_unlock(&leaf->lock);
        return 0;
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
    if (leaf->connected) {
        cmq_mutex_unlock(&leaf->lock);
        cmq_mutex_unlock(&leaf->hub_io_lock);
        close(fd);
        return 0;
    }
    leaf->hub_fd = fd;
    leaf->connected = 1;
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
        if (flen == 0 || write_all(fd, frame, flen) != 0) {
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
    int c = leaf->connected;
    cmq_mutex_unlock(&leaf->lock);
    return c;
}

int cmq_leaf_subscribe(cmq_leaf_node_t *leaf, const char *subject) {
    if (!leaf || !subject) return -1;
    size_t slen = strlen(subject);
    if (slen == 0 || slen >= 256) return -1;

    cmq_mutex_lock(&leaf->lock);
    if (leaf->sub_count >= CMQ_LEAF_MAX_SUBS) {
        cmq_mutex_unlock(&leaf->lock);
        return -1;
    }
    for (size_t i = 0; i < leaf->sub_count; i++) {
        if (strcmp(leaf->subs[i], subject) == 0) {
            cmq_mutex_unlock(&leaf->lock);
            return 0;
        }
    }
    char *copy = strdup(subject);
    if (!copy) {
        cmq_mutex_unlock(&leaf->lock);
        return -1;
    }
    /* Claim local slot before hub I/O so capacity races cannot desync. */
    size_t idx = leaf->sub_count;
    uint32_t sub_id = leaf->next_sub_id++;
    leaf->subs[idx] = copy;
    leaf->sub_ids[idx] = sub_id;
    leaf->sub_count++;
    int hub_fd = leaf->hub_fd;
    int connected = leaf->connected;
    cmq_mutex_unlock(&leaf->lock);

    if (connected && hub_fd >= 0) {
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
        cmq_mutex_lock(&leaf->hub_io_lock);
        cmq_mutex_lock(&leaf->lock);
        int still = (leaf->connected && leaf->hub_fd == hub_fd);
        cmq_mutex_unlock(&leaf->lock);
        int wr = (still && flen > 0) ? write_all(hub_fd, frame, flen) : -1;
        cmq_mutex_unlock(&leaf->hub_io_lock);
        if (flen == 0 || wr != 0) {
            cmq_mutex_lock(&leaf->lock);
            for (size_t i = 0; i < leaf->sub_count; i++) {
                if (leaf->subs[i] == copy) {
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
            return -1;
        }
    }
    return 0;
}

int cmq_leaf_unsubscribe(cmq_leaf_node_t *leaf, const char *subject) {
    if (!leaf || !subject) return -1;
    cmq_mutex_lock(&leaf->lock);
    for (size_t i = 0; i < leaf->sub_count; i++) {
        if (strcmp(leaf->subs[i], subject) == 0) {
            uint32_t sub_id = leaf->sub_ids[i];
            int hub_fd = leaf->hub_fd;
            int connected = leaf->connected;
            char *kept = leaf->subs[i];
            cmq_mutex_unlock(&leaf->lock);

            /* Notify hub first — avoid ghost interest if write fails. */
            if (connected && hub_fd >= 0) {
                uint8_t payload[4];
                payload[0] = (uint8_t)(sub_id >> 24);
                payload[1] = (uint8_t)(sub_id >> 16);
                payload[2] = (uint8_t)(sub_id >> 8);
                payload[3] = (uint8_t)sub_id;
                uint8_t frame[16];
                size_t flen = cmq_frame_encode(frame, sizeof(frame),
                                                CMQ_OP_UNSUBSCRIBE, 0,
                                                payload, 4);
                cmq_mutex_lock(&leaf->hub_io_lock);
                cmq_mutex_lock(&leaf->lock);
                int still = (leaf->connected && leaf->hub_fd == hub_fd);
                cmq_mutex_unlock(&leaf->lock);
                int wr = (still && flen > 0) ? write_all(hub_fd, frame, flen) : -1;
                cmq_mutex_unlock(&leaf->hub_io_lock);
                if (flen == 0 || wr != 0)
                    return -1;
            }

            cmq_mutex_lock(&leaf->lock);
            for (size_t j = 0; j < leaf->sub_count; j++) {
                if (leaf->subs[j] == kept ||
                    (leaf->subs[j] && strcmp(leaf->subs[j], subject) == 0 &&
                     leaf->sub_ids[j] == sub_id)) {
                    free(leaf->subs[j]);
                    memmove(&leaf->subs[j], &leaf->subs[j + 1],
                            (leaf->sub_count - j - 1) * sizeof(char *));
                    memmove(&leaf->sub_ids[j], &leaf->sub_ids[j + 1],
                            (leaf->sub_count - j - 1) * sizeof(uint32_t));
                    leaf->sub_count--;
                    break;
                }
            }
            cmq_mutex_unlock(&leaf->lock);
            return 0;
        }
    }
    cmq_mutex_unlock(&leaf->lock);
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
