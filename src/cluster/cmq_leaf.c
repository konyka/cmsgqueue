#define _POSIX_C_SOURCE 200809L
#include "cmq_leaf.h"
#include "cmq_thread.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define CMQ_LEAF_MAX_SUBS 1024

struct cmq_leaf_node {
    char hub_addr[CMQ_NODE_ADDR_SIZE];
    int hub_port;
    int hub_fd;
    int connected;

    char *subs[CMQ_LEAF_MAX_SUBS];
    size_t sub_count;

    cmq_leaf_conn_t leaves[CMQ_LEAF_MAX_CONNECTIONS];
    size_t leaf_count;

    cmq_mutex_t lock;
};

cmq_leaf_node_t *cmq_leaf_create(const char *hub_addr, int hub_port) {
    if (!hub_addr) return NULL;
    cmq_leaf_node_t *l = calloc(1, sizeof(cmq_leaf_node_t));
    if (!l) return NULL;
    strncpy(l->hub_addr, hub_addr, CMQ_NODE_ADDR_SIZE - 1);
    l->hub_port = hub_port;
    l->hub_fd = -1;
    l->connected = 0;
    l->sub_count = 0;
    l->leaf_count = 0;
    cmq_mutex_init(&l->lock);
    return l;
}

void cmq_leaf_destroy(cmq_leaf_node_t *leaf) {
    if (!leaf) return;
    if (leaf->hub_fd >= 0) close(leaf->hub_fd);
    for (size_t i = 0; i < leaf->leaf_count; i++) {
        if (leaf->leaves[i].fd >= 0) close(leaf->leaves[i].fd);
    }
    for (size_t i = 0; i < leaf->sub_count; i++) {
        free(leaf->subs[i]);
    }
    cmq_mutex_destroy(&leaf->lock);
    free(leaf);
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

    if (leaf->connected) {
        cmq_mutex_unlock(&leaf->lock);
        return 0;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        cmq_mutex_unlock(&leaf->lock);
        return -1;
    }

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)leaf->hub_port);
    inet_pton(AF_INET, leaf->hub_addr, &sa.sin_addr);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        cmq_mutex_unlock(&leaf->lock);
        return -1;
    }

    leaf->hub_fd = fd;
    leaf->connected = 1;

    cmq_mutex_unlock(&leaf->lock);
    return 0;
}

int cmq_leaf_disconnect(cmq_leaf_node_t *leaf) {
    if (!leaf) return -1;
    cmq_mutex_lock(&leaf->lock);
    if (leaf->hub_fd >= 0) close(leaf->hub_fd);
    leaf->hub_fd = -1;
    leaf->connected = 0;
    cmq_mutex_unlock(&leaf->lock);
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
    leaf->subs[leaf->sub_count] = strdup(subject);
    if (!leaf->subs[leaf->sub_count]) {
        cmq_mutex_unlock(&leaf->lock);
        return -1;
    }
    leaf->sub_count++;
    cmq_mutex_unlock(&leaf->lock);
    return 0;
}

int cmq_leaf_unsubscribe(cmq_leaf_node_t *leaf, const char *subject) {
    if (!leaf || !subject) return -1;
    cmq_mutex_lock(&leaf->lock);
    for (size_t i = 0; i < leaf->sub_count; i++) {
        if (strcmp(leaf->subs[i], subject) == 0) {
            free(leaf->subs[i]);
            memmove(&leaf->subs[i], &leaf->subs[i + 1],
                    (leaf->sub_count - i - 1) * sizeof(char *));
            leaf->sub_count--;
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
    if (!leaf || fd < 0 || !leaf_id) return -1;
    cmq_mutex_lock(&leaf->lock);
    if (leaf->leaf_count >= CMQ_LEAF_MAX_CONNECTIONS) {
        cmq_mutex_unlock(&leaf->lock);
        return -1;
    }
    cmq_leaf_conn_t *c = &leaf->leaves[leaf->leaf_count++];
    strncpy(c->leaf_id, leaf_id, CMQ_NODE_ID_SIZE - 1);
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
