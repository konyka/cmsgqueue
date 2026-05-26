#ifndef CMQ_EV_H
#define CMQ_EV_H

#include <stdint.h>
#include <stddef.h>
#include "cmq_platform.h"

#define CMQ_EV_READ    0x01
#define CMQ_EV_WRITE   0x02
#define CMQ_EV_ERROR   0x04
#define CMQ_EV_TIMER   0x08

#define CMQ_EV_INVALID -1

typedef struct cmq_ev_loop cmq_ev_loop_t;

typedef void (*cmq_ev_cb_t)(int fd, int events, void *data);
typedef void (*cmq_ev_tick_t)(void *data);

typedef struct {
    int timer_id;
    uint64_t expire_ms;
    uint64_t interval_ms;
    cmq_ev_cb_t cb;
    void *data;
    int repeat;
    int active;
} cmq_ev_timer_t;

cmq_ev_loop_t *cmq_ev_loop_create(int max_events);
void cmq_ev_loop_destroy(cmq_ev_loop_t *loop);

int cmq_ev_add(cmq_ev_loop_t *loop, int fd, int events, cmq_ev_cb_t cb, void *data);
int cmq_ev_mod(cmq_ev_loop_t *loop, int fd, int events, cmq_ev_cb_t cb, void *data);
int cmq_ev_del(cmq_ev_loop_t *loop, int fd);

int cmq_ev_timer_add(cmq_ev_loop_t *loop, uint64_t delay_ms, uint64_t interval_ms, cmq_ev_cb_t cb, void *data);
int cmq_ev_timer_del(cmq_ev_loop_t *loop, int timer_id);

int cmq_ev_run(cmq_ev_loop_t *loop, int timeout_ms);
void cmq_ev_stop(cmq_ev_loop_t *loop);
void cmq_ev_set_post_tick(cmq_ev_loop_t *loop, cmq_ev_tick_t tick, void *data);

int cmq_ev_fd(cmq_ev_loop_t *loop);

#endif
