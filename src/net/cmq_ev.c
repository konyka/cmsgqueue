#define _POSIX_C_SOURCE 200809L
#include "cmq_ev.h"
#include "cmq_types.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/eventfd.h>

#if CMQ_OS_LINUX
#include <sys/epoll.h>
#elif CMQ_OS_MACOS || CMQ_OS_FREEBSD || CMQ_OS_OPENBSD || CMQ_OS_NETBSD
#include <sys/event.h>
#endif

#define CMQ_EV_MAX_EVENTS 64
#define CMQ_EV_MAX_TIMERS 256
#define CMQ_EV_INITIAL_WATCHERS 64

typedef struct {
    int fd;
    int events;
    cmq_ev_cb_t cb;
    void *data;
} cmq_ev_watcher_t;

struct cmq_ev_loop {
    int backend_fd;
    int wakeup_fd;
    int running;
    int next_timer_id;
    cmq_ev_watcher_t *watchers;
    int watchers_cap;
    cmq_ev_timer_t timers[CMQ_EV_MAX_TIMERS];
};

static uint64_t cmq_ev_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

cmq_ev_loop_t *cmq_ev_loop_create(int max_events) {
    (void)max_events;
    cmq_ev_loop_t *loop = calloc(1, sizeof(cmq_ev_loop_t));
    if (!loop) return NULL;

    loop->watchers_cap = CMQ_EV_INITIAL_WATCHERS;
    loop->watchers = calloc((size_t)loop->watchers_cap, sizeof(cmq_ev_watcher_t));
    if (!loop->watchers) {
        free(loop);
        return NULL;
    }

    for (int i = 0; i < loop->watchers_cap; i++) {
        loop->watchers[i].fd = -1;
    }

    for (int i = 0; i < CMQ_EV_MAX_TIMERS; i++) {
        loop->timers[i].active = 0;
    }

    loop->running = 0;
    loop->next_timer_id = 1;

#if CMQ_OS_LINUX
    loop->backend_fd = epoll_create1(EPOLL_CLOEXEC);
#elif CMQ_OS_MACOS || CMQ_OS_FREEBSD || CMQ_OS_OPENBSD || CMQ_OS_NETBSD
    loop->backend_fd = kqueue();
#else
    loop->backend_fd = -1;
#endif

    if (loop->backend_fd < 0) {
        free(loop->watchers);
        free(loop);
        return NULL;
    }

    loop->wakeup_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (loop->wakeup_fd >= 0) {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.fd = loop->wakeup_fd;
        epoll_ctl(loop->backend_fd, EPOLL_CTL_ADD, loop->wakeup_fd, &ev);
    }

    return loop;
}

void cmq_ev_loop_destroy(cmq_ev_loop_t *loop) {
    if (!loop) return;
    if (loop->wakeup_fd >= 0) close(loop->wakeup_fd);
    if (loop->backend_fd >= 0) close(loop->backend_fd);
    free(loop->watchers);
    free(loop);
}

static int cmq_ev_ensure_watcher(cmq_ev_loop_t *loop, int fd) {
    if (fd >= loop->watchers_cap) {
        int new_cap = loop->watchers_cap;
        while (new_cap <= fd) new_cap *= 2;
        cmq_ev_watcher_t *new_w = realloc(loop->watchers, (size_t)new_cap * sizeof(cmq_ev_watcher_t));
        if (!new_w) return -1;
        for (int i = loop->watchers_cap; i < new_cap; i++) {
            new_w[i].fd = -1;
        }
        loop->watchers = new_w;
        loop->watchers_cap = new_cap;
    }
    return 0;
}

#if CMQ_OS_LINUX

static int cmq_to_epoll_events(int events) {
    int ep = 0;
    if (events & CMQ_EV_READ)  ep |= EPOLLIN;
    if (events & CMQ_EV_WRITE) ep |= EPOLLOUT;
    if (events & CMQ_EV_ERROR) ep |= EPOLLERR;
    return ep;
}

static int epoll_to_cmq_events(int ep) {
    int events = 0;
    if (ep & (EPOLLIN | EPOLLHUP))  events |= CMQ_EV_READ;
    if (ep & EPOLLOUT) events |= CMQ_EV_WRITE;
    if (ep & EPOLLERR) events |= CMQ_EV_ERROR;
    return events;
}

int cmq_ev_add(cmq_ev_loop_t *loop, int fd, int events, cmq_ev_cb_t cb, void *data) {
    if (!loop || fd < 0) return -1;
    if (cmq_ev_ensure_watcher(loop, fd) != 0) return -1;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = (uint32_t)cmq_to_epoll_events(events);
    ev.data.fd = fd;

    if (epoll_ctl(loop->backend_fd, EPOLL_CTL_ADD, fd, &ev) != 0)
        return -1;

    loop->watchers[fd].fd = fd;
    loop->watchers[fd].events = events;
    loop->watchers[fd].cb = cb;
    loop->watchers[fd].data = data;
    return 0;
}

int cmq_ev_mod(cmq_ev_loop_t *loop, int fd, int events, cmq_ev_cb_t cb, void *data) {
    if (!loop || fd < 0 || fd >= loop->watchers_cap) return -1;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = (uint32_t)cmq_to_epoll_events(events);
    ev.data.fd = fd;

    if (epoll_ctl(loop->backend_fd, EPOLL_CTL_MOD, fd, &ev) != 0)
        return -1;

    loop->watchers[fd].events = events;
    loop->watchers[fd].cb = cb;
    loop->watchers[fd].data = data;
    return 0;
}

int cmq_ev_del(cmq_ev_loop_t *loop, int fd) {
    if (!loop || fd < 0 || fd >= loop->watchers_cap) return -1;
    if (loop->watchers[fd].fd < 0) return -1;

    epoll_ctl(loop->backend_fd, EPOLL_CTL_DEL, fd, NULL);
    loop->watchers[fd].fd = -1;
    return 0;
}

int cmq_ev_run(cmq_ev_loop_t *loop, int timeout_ms) {
    if (!loop) return -1;
    loop->running = 1;

    struct epoll_event events[CMQ_EV_MAX_EVENTS];

    while (loop->running) {
        int wait_ms = timeout_ms;

        uint64_t now = cmq_ev_now_ms();
        for (int i = 0; i < CMQ_EV_MAX_TIMERS; i++) {
            cmq_ev_timer_t *t = &loop->timers[i];
            if (!t->active) continue;
            int64_t diff = (int64_t)(t->expire_ms) - (int64_t)now;
            if (diff <= 0) diff = 0;
            if (diff < wait_ms || wait_ms < 0) wait_ms = (int)diff;
        }

        int nfds = epoll_wait(loop->backend_fd, events, CMQ_EV_MAX_EVENTS, wait_ms);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            return -1;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            if (fd == loop->wakeup_fd) {
                uint64_t val;
                read(fd, &val, sizeof(val));
                continue;
            }
            if (fd >= 0 && fd < loop->watchers_cap && loop->watchers[fd].cb) {
                int ev = epoll_to_cmq_events((int)events[i].events);
                loop->watchers[fd].cb(fd, ev, loop->watchers[fd].data);
            }
        }

        now = cmq_ev_now_ms();
        for (int i = 0; i < CMQ_EV_MAX_TIMERS; i++) {
            cmq_ev_timer_t *t = &loop->timers[i];
            if (!t->active) continue;
            if (t->expire_ms <= now) {
                if (t->cb) t->cb(t->timer_id, CMQ_EV_TIMER, t->data);
                if (t->repeat) {
                    t->expire_ms = now + t->interval_ms;
                } else {
                    t->active = 0;
                }
            }
        }

        if (timeout_ms >= 0) break;
    }
    return 0;
}

#elif CMQ_OS_MACOS || CMQ_OS_FREEBSD || CMQ_OS_OPENBSD || CMQ_OS_NETBSD

static short cmq_to_kqueue_filter(int events) {
    if (events & CMQ_EV_READ) return EVFILT_READ;
    if (events & CMQ_EV_WRITE) return EVFILT_WRITE;
    return EVFILT_READ;
}

static int kqueue_to_cmq_events(short filter, int flags) {
    int events = 0;
    if (filter == EVFILT_READ)  events |= CMQ_EV_READ;
    if (filter == EVFILT_WRITE) events |= CMQ_EV_WRITE;
    if (flags & EV_ERROR) events |= CMQ_EV_ERROR;
    return events;
}

int cmq_ev_add(cmq_ev_loop_t *loop, int fd, int events, cmq_ev_cb_t cb, void *data) {
    if (!loop || fd < 0) return -1;
    if (cmq_ev_ensure_watcher(loop, fd) != 0) return -1;

    struct kevent ev;
    short filter = cmq_to_kqueue_filter(events);
    EV_SET(&ev, (uintptr_t)fd, filter, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (kevent(loop->backend_fd, &ev, 1, NULL, 0, NULL) != 0)
        return -1;

    loop->watchers[fd].fd = fd;
    loop->watchers[fd].events = events;
    loop->watchers[fd].cb = cb;
    loop->watchers[fd].data = data;
    return 0;
}

int cmq_ev_mod(cmq_ev_loop_t *loop, int fd, int events, cmq_ev_cb_t cb, void *data) {
    if (!loop || fd < 0 || fd >= loop->watchers_cap) return -1;

    struct kevent ev[2];
    int n = 0;

    if (loop->watchers[fd].events & CMQ_EV_READ)
        EV_SET(&ev[n++], (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    if (loop->watchers[fd].events & CMQ_EV_WRITE)
        EV_SET(&ev[n++], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);

    if (n > 0) kevent(loop->backend_fd, ev, n, NULL, 0, NULL);

    short filter = cmq_to_kqueue_filter(events);
    EV_SET(&ev[0], (uintptr_t)fd, filter, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (kevent(loop->backend_fd, &ev[0], 1, NULL, 0, NULL) != 0)
        return -1;

    loop->watchers[fd].events = events;
    loop->watchers[fd].cb = cb;
    loop->watchers[fd].data = data;
    return 0;
}

int cmq_ev_del(cmq_ev_loop_t *loop, int fd) {
    if (!loop || fd < 0 || fd >= loop->watchers_cap) return -1;
    if (loop->watchers[fd].fd < 0) return -1;

    struct kevent ev[2];
    int n = 0;
    if (loop->watchers[fd].events & CMQ_EV_READ)
        EV_SET(&ev[n++], (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    if (loop->watchers[fd].events & CMQ_EV_WRITE)
        EV_SET(&ev[n++], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    if (n > 0) kevent(loop->backend_fd, ev, n, NULL, 0, NULL);

    loop->watchers[fd].fd = -1;
    return 0;
}

int cmq_ev_run(cmq_ev_loop_t *loop, int timeout_ms) {
    if (!loop) return -1;
    loop->running = 1;

    struct kevent events[CMQ_EV_MAX_EVENTS];

    while (loop->running) {
        struct timespec ts;
        struct timespec *tsp = NULL;
        if (timeout_ms >= 0) {
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
            tsp = &ts;
        }

        int nfds = kevent(loop->backend_fd, NULL, 0, events, CMQ_EV_MAX_EVENTS, tsp);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            return -1;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = (int)events[i].ident;
            if (fd >= 0 && fd < loop->watchers_cap && loop->watchers[fd].cb) {
                int ev = kqueue_to_cmq_events(events[i].filter, (int)events[i].flags);
                loop->watchers[fd].cb(fd, ev, loop->watchers[fd].data);
            }
        }

        uint64_t now = cmq_ev_now_ms();
        for (int i = 0; i < CMQ_EV_MAX_TIMERS; i++) {
            cmq_ev_timer_t *t = &loop->timers[i];
            if (!t->active) continue;
            if (t->expire_ms <= now) {
                if (t->cb) t->cb(t->timer_id, CMQ_EV_TIMER, t->data);
                if (t->repeat) {
                    t->expire_ms = now + t->interval_ms;
                } else {
                    t->active = 0;
                }
            }
        }

        if (timeout_ms >= 0) break;
    }
    return 0;
}

#else

int cmq_ev_add(cmq_ev_loop_t *loop, int fd, int events, cmq_ev_cb_t cb, void *data) { (void)loop;(void)fd;(void)events;(void)cb;(void)data; return -1; }
int cmq_ev_mod(cmq_ev_loop_t *loop, int fd, int events, cmq_ev_cb_t cb, void *data) { (void)loop;(void)fd;(void)events;(void)cb;(void)data; return -1; }
int cmq_ev_del(cmq_ev_loop_t *loop, int fd) { (void)loop;(void)fd; return -1; }
int cmq_ev_run(cmq_ev_loop_t *loop, int timeout_ms) { (void)loop;(void)timeout_ms; return -1; }

#endif

int cmq_ev_timer_add(cmq_ev_loop_t *loop, uint64_t delay_ms, uint64_t interval_ms, cmq_ev_cb_t cb, void *data) {
    if (!loop) return -1;
    for (int i = 0; i < CMQ_EV_MAX_TIMERS; i++) {
        if (!loop->timers[i].active) {
            uint64_t now = cmq_ev_now_ms();
            loop->timers[i].timer_id = loop->next_timer_id++;
            loop->timers[i].expire_ms = now + delay_ms;
            loop->timers[i].interval_ms = interval_ms;
            loop->timers[i].cb = cb;
            loop->timers[i].data = data;
            loop->timers[i].repeat = (interval_ms > 0) ? 1 : 0;
            loop->timers[i].active = 1;
            return loop->timers[i].timer_id;
        }
    }
    return -1;
}

int cmq_ev_timer_del(cmq_ev_loop_t *loop, int timer_id) {
    if (!loop) return -1;
    for (int i = 0; i < CMQ_EV_MAX_TIMERS; i++) {
        if (loop->timers[i].active && loop->timers[i].timer_id == timer_id) {
            loop->timers[i].active = 0;
            return 0;
        }
    }
    return -1;
}

void cmq_ev_stop(cmq_ev_loop_t *loop) {
    if (!loop) return;
    loop->running = 0;
    if (loop->wakeup_fd >= 0) {
        uint64_t val = 1;
        write(loop->wakeup_fd, &val, sizeof(val));
    }
}

int cmq_ev_fd(cmq_ev_loop_t *loop) {
    if (!loop) return -1;
    return loop->backend_fd;
}
