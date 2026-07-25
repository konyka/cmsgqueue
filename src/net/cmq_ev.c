#define _POSIX_C_SOURCE 200809L
#include "cmq_ev.h"
#include "cmq_types.h"
#include "cmq_atomic.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdatomic.h>

#if CMQ_OS_LINUX
#include <sys/epoll.h>
#include <sys/eventfd.h>
#elif CMQ_OS_MACOS || CMQ_OS_FREEBSD || CMQ_OS_OPENBSD || CMQ_OS_NETBSD
#include <sys/event.h>
#endif

#include "cmq_thread.h"
#include <sys/resource.h>

#define CMQ_EV_MAX_EVENTS 64
#define CMQ_EV_MAX_TIMERS 256
#define CMQ_EV_INITIAL_WATCHERS 64
/* Cap soft-limit pre-size so a huge RLIMIT_NOFILE cannot OOM the process. */
#define CMQ_EV_WATCHERS_CAP_MAX 1048576

typedef struct {
    int fd;
    int events;
    cmq_ev_cb_t cb;
    void *data;
    uint32_t gen; /* bumped on publish; 0 = cleared — drop stale wait events */
} cmq_ev_watcher_t;

/* epoll data.u64 / kqueue udata: bind interest to a watcher generation. */
static uint64_t ev_pack_fd_gen(int fd, uint32_t gen) {
    return ((uint64_t)gen << 32) | (uint32_t)fd;
}

static void ev_unpack_fd_gen(uint64_t u, int *fd, uint32_t *gen) {
    *fd = (int)(uint32_t)u;
    *gen = (uint32_t)(u >> 32);
}

/* running: IDLE↔RUN via CAS; STOP sticky until run claims or exits.
   Plain running=0/1 lost a stop that raced before run set running=1. */
enum {
    CMQ_EV_RS_IDLE = 0,
    CMQ_EV_RS_RUN  = 1,
    CMQ_EV_RS_STOP = 2
};

struct cmq_ev_loop {
    int backend_fd;
    int wakeup_fd;   /* eventfd, or pipe read end */
    int wakeup_wfd;  /* write end; same as wakeup_fd for eventfd */
    cmq_atomic_int running;
    int next_timer_id;
    cmq_ev_watcher_t *watchers;
    int watchers_cap;
    cmq_mutex_t watchers_lock; /* publish/clear vs run snapshot (accept→worker) */
    cmq_ev_timer_t timers[CMQ_EV_MAX_TIMERS];
    cmq_mutex_t timers_lock; /* timer_add/del vs run scan/fire */
    cmq_ev_tick_t post_tick;
    void *post_tick_data;
    atomic_int in_flight; /* run/add/mod/del/timer vs destroy */
    atomic_int dying;
    atomic_int in_timer_cb; /* run thread inside timer callback (reentrant del) */
    atomic_int run_owned;   /* 1 while cmq_ev_run owns the loop */
    cmq_thread_t run_tid;   /* valid when run_owned — foil cross-thread del */
};

/* 0 = enter wait loop; -1 = pending stop (or nested run) — caller returns 0. */
static int ev_run_claim(cmq_ev_loop_t *loop) {
    for (;;) {
        int st = cmq_atomic_load_int(&loop->running, CMQ_ATOMIC_ACQUIRE);
        if (st == CMQ_EV_RS_STOP) {
            cmq_atomic_store_int(&loop->running, CMQ_EV_RS_IDLE, CMQ_ATOMIC_RELEASE);
            return -1;
        }
        if (st == CMQ_EV_RS_RUN)
            return -1;
        int expected = CMQ_EV_RS_IDLE;
        if (cmq_atomic_cas_int(&loop->running, &expected, CMQ_EV_RS_RUN,
                               CMQ_ATOMIC_ACQ_REL))
            return 0;
    }
}

/* Timed/error exit must CAS RUN→IDLE only — a racing STOP must stick.
   Stop-driven exit consumes STOP→IDLE so the next claim can start. */
static void ev_run_release(cmq_ev_loop_t *loop) {
    int st = cmq_atomic_load_int(&loop->running, CMQ_ATOMIC_ACQUIRE);
    if (st == CMQ_EV_RS_STOP) {
        int expected = CMQ_EV_RS_STOP;
        (void)cmq_atomic_cas_int(&loop->running, &expected, CMQ_EV_RS_IDLE,
                                 CMQ_ATOMIC_ACQ_REL);
        return;
    }
    if (st == CMQ_EV_RS_RUN) {
        int expected = CMQ_EV_RS_RUN;
        (void)cmq_atomic_cas_int(&loop->running, &expected, CMQ_EV_RS_IDLE,
                                 CMQ_ATOMIC_ACQ_REL);
    }
}

static void ev_run_enter(cmq_ev_loop_t *loop) {
    loop->run_tid = cmq_thread_self();
    atomic_store_explicit(&loop->run_owned, 1, memory_order_release);
}

static void ev_run_leave(cmq_ev_loop_t *loop) {
    atomic_store_explicit(&loop->run_owned, 0, memory_order_release);
}

/* True only for reentrant del from the run thread's timer callback.
   Global in_timer_cb alone must not skip wait for other threads. */
static int ev_timer_del_reentrant(cmq_ev_loop_t *loop) {
    if (!atomic_load_explicit(&loop->in_timer_cb, memory_order_acquire))
        return 0;
    if (!atomic_load_explicit(&loop->run_owned, memory_order_acquire))
        return 0;
    return pthread_equal(cmq_thread_self(), loop->run_tid);
}

/* Fire due timers. One-shot disarms under lock before unlock+cb so concurrent
   timer_del cannot return success while the callback still runs. Repeat timers
   use firing/cancelled; cross-thread del waits until firing clears (reentrant
   del only when on the run thread inside the timer callback). */
static void cmq_ev_timers_dispatch(cmq_ev_loop_t *loop, uint64_t now) {
    for (int i = 0; i < CMQ_EV_MAX_TIMERS; i++) {
        cmq_ev_cb_t cb = NULL;
        void *data = NULL;
        int tid = 0;
        int was_repeat = 0;
        cmq_mutex_lock(&loop->timers_lock);
        cmq_ev_timer_t *t = &loop->timers[i];
        if (!t->active || t->expire_ms > now) {
            cmq_mutex_unlock(&loop->timers_lock);
            continue;
        }
        cb = t->cb;
        data = t->data;
        tid = t->timer_id;
        was_repeat = t->repeat;
        t->firing = 1;
        t->cancelled = 0;
        if (!was_repeat)
            t->active = 0;
        cmq_mutex_unlock(&loop->timers_lock);

        atomic_store_explicit(&loop->in_timer_cb, 1, memory_order_release);
        if (cb) cb(tid, CMQ_EV_TIMER, data);
        atomic_store_explicit(&loop->in_timer_cb, 0, memory_order_release);

        cmq_mutex_lock(&loop->timers_lock);
        t->firing = 0;
        if (t->timer_id != tid) {
            /* Slot reused under a different id — leave the new timer alone. */
        } else if (t->cancelled || !was_repeat) {
            t->active = 0;
            t->repeat = 0;
            t->cb = NULL;
            t->data = NULL;
            t->cancelled = 0;
            t->timer_id = 0; /* free id for wrap; del must not hit tombstones */
        } else if (t->active) {
            if (t->interval_ms > UINT64_MAX - now)
                t->expire_ms = UINT64_MAX;
            else
                t->expire_ms = now + t->interval_ms;
        } else {
            t->repeat = 0;
            t->cb = NULL;
            t->data = NULL;
            t->timer_id = 0;
        }
        cmq_mutex_unlock(&loop->timers_lock);
    }
}

static int ev_begin_op(cmq_ev_loop_t *loop) {
    if (atomic_load_explicit(&loop->dying, memory_order_acquire))
        return -1;
    atomic_fetch_add_explicit(&loop->in_flight, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&loop->dying, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&loop->in_flight, 1, memory_order_acq_rel);
        return -1;
    }
    return 0;
}

static void ev_end_op(cmq_ev_loop_t *loop) {
    atomic_fetch_sub_explicit(&loop->in_flight, 1, memory_order_acq_rel);
}

static uint64_t cmq_ev_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int cmq_ev_watchers_presize(void) {
    int cap = CMQ_EV_INITIAL_WATCHERS;
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
        if (rl.rlim_cur > (rlim_t)CMQ_EV_WATCHERS_CAP_MAX)
            cap = CMQ_EV_WATCHERS_CAP_MAX;
        else if (rl.rlim_cur > (rlim_t)cap)
            cap = (int)rl.rlim_cur;
    }
    if (cap < 1024)
        cap = 1024;
    return cap;
}

cmq_ev_loop_t *cmq_ev_loop_create(int max_events) {
    (void)max_events;
    cmq_ev_loop_t *loop = calloc(1, sizeof(cmq_ev_loop_t));
    if (!loop) return NULL;

    /* Pre-size to RLIMIT_NOFILE so cross-thread cmq_ev_add (accept→worker)
       never reallocs watchers under a concurrent cmq_ev_run. */
    loop->watchers_cap = cmq_ev_watchers_presize();
    loop->watchers = calloc((size_t)loop->watchers_cap, sizeof(cmq_ev_watcher_t));
    if (!loop->watchers) {
        free(loop);
        return NULL;
    }

    for (int i = 0; i < loop->watchers_cap; i++) {
        loop->watchers[i].fd = -1;
    }
    cmq_mutex_init(&loop->watchers_lock);
    cmq_mutex_init(&loop->timers_lock);

    for (int i = 0; i < CMQ_EV_MAX_TIMERS; i++) {
        loop->timers[i].active = 0;
    }

    cmq_atomic_store_int(&loop->running, 0, CMQ_ATOMIC_RELAXED);
    atomic_init(&loop->in_flight, 0);
    atomic_init(&loop->dying, 0);
    atomic_init(&loop->run_owned, 0);
    atomic_init(&loop->in_timer_cb, 0);
    loop->next_timer_id = 1;

#if CMQ_OS_LINUX
    loop->backend_fd = epoll_create1(EPOLL_CLOEXEC);
#elif CMQ_OS_MACOS || CMQ_OS_FREEBSD || CMQ_OS_OPENBSD || CMQ_OS_NETBSD
    loop->backend_fd = kqueue();
#else
    loop->backend_fd = -1;
#endif

    if (loop->backend_fd < 0) {
        cmq_mutex_destroy(&loop->timers_lock);
        cmq_mutex_destroy(&loop->watchers_lock);
        free(loop->watchers);
        free(loop);
        return NULL;
    }

    loop->wakeup_fd = -1;
    loop->wakeup_wfd = -1;

#if CMQ_OS_LINUX
    /* Fail-closed: workers block in epoll_wait(-1) with no timer; without a
       registered wakeup, cmq_ev_stop cannot interrupt an idle loop. */
    loop->wakeup_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (loop->wakeup_fd < 0)
        goto fail_loop;
    {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.u64 = ev_pack_fd_gen(loop->wakeup_fd, 0);
        if (epoll_ctl(loop->backend_fd, EPOLL_CTL_ADD, loop->wakeup_fd, &ev) != 0)
            goto fail_loop;
        loop->wakeup_wfd = loop->wakeup_fd;
    }
#elif CMQ_OS_MACOS || CMQ_OS_FREEBSD || CMQ_OS_OPENBSD || CMQ_OS_NETBSD
    {
        int fds[2];
        if (pipe(fds) != 0)
            goto fail_loop;
        int flags0 = fcntl(fds[0], F_GETFL, 0);
        int flags1 = fcntl(fds[1], F_GETFL, 0);
        if (flags0 >= 0) fcntl(fds[0], F_SETFL, flags0 | O_NONBLOCK);
        if (flags1 >= 0) fcntl(fds[1], F_SETFL, flags1 | O_NONBLOCK);
        struct kevent kev;
        EV_SET(&kev, (uintptr_t)fds[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
        if (kevent(loop->backend_fd, &kev, 1, NULL, 0, NULL) != 0) {
            close(fds[0]);
            close(fds[1]);
            goto fail_loop;
        }
        loop->wakeup_fd = fds[0];
        loop->wakeup_wfd = fds[1];
    }
#endif

    return loop;

#if CMQ_OS_LINUX || CMQ_OS_MACOS || CMQ_OS_FREEBSD || CMQ_OS_OPENBSD || \
    CMQ_OS_NETBSD
fail_loop:
    if (loop->wakeup_fd >= 0) close(loop->wakeup_fd);
    if (loop->wakeup_wfd >= 0 && loop->wakeup_wfd != loop->wakeup_fd)
        close(loop->wakeup_wfd);
    if (loop->backend_fd >= 0) close(loop->backend_fd);
    cmq_mutex_destroy(&loop->timers_lock);
    cmq_mutex_destroy(&loop->watchers_lock);
    free(loop->watchers);
    free(loop);
    return NULL;
#endif
}

/* Write wakeup byte/eventfd. Caller must hold in_flight or be destroy
   after dying=1 (no concurrent close until in_flight drains). */
static void ev_wakeup_raw(cmq_ev_loop_t *loop) {
    if (!loop || loop->wakeup_wfd < 0) return;
#if CMQ_OS_LINUX
    uint64_t val = 1;
    for (;;) {
        ssize_t n = write(loop->wakeup_wfd, &val, sizeof(val));
        if (n == (ssize_t)sizeof(val)) return;
        if (n < 0 && errno == EINTR) continue;
        /* Saturated eventfd is already readable — treat as woken. */
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        return;
    }
#else
    char c = 1;
    for (;;) {
        ssize_t n = write(loop->wakeup_wfd, &c, 1);
        if (n == 1) return;
        if (n < 0 && errno == EINTR) continue;
        /* Pipe full → pending bytes already wake the reader. */
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        return;
    }
#endif
}

void cmq_ev_loop_destroy(cmq_ev_loop_t *loop) {
    if (!loop) return;
    atomic_store_explicit(&loop->dying, 1, memory_order_release);
    /* Cannot use public stop/wakeup (begin_op fails once dying). */
    cmq_atomic_store_int(&loop->running, CMQ_EV_RS_STOP, CMQ_ATOMIC_RELEASE);
    ev_wakeup_raw(loop);
    while (atomic_load_explicit(&loop->in_flight, memory_order_acquire) > 0) {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }
    if (loop->wakeup_fd >= 0) close(loop->wakeup_fd);
    if (loop->wakeup_wfd >= 0 && loop->wakeup_wfd != loop->wakeup_fd)
        close(loop->wakeup_wfd);
    if (loop->backend_fd >= 0) close(loop->backend_fd);
    cmq_mutex_destroy(&loop->timers_lock);
    cmq_mutex_destroy(&loop->watchers_lock);
    free(loop->watchers);
    free(loop);
}

static uint32_t watcher_publish(cmq_ev_loop_t *loop, int fd, int events,
                                 cmq_ev_cb_t cb, void *data) {
    cmq_mutex_lock(&loop->watchers_lock);
    uint32_t g = loop->watchers[fd].gen + 1;
    if (g == 0) g = 1;
    loop->watchers[fd].fd = fd;
    loop->watchers[fd].events = events;
    loop->watchers[fd].cb = cb;
    loop->watchers[fd].data = data;
    loop->watchers[fd].gen = g;
    cmq_mutex_unlock(&loop->watchers_lock);
    return g;
}

static void watcher_clear(cmq_ev_loop_t *loop, int fd) {
    cmq_mutex_lock(&loop->watchers_lock);
    loop->watchers[fd].fd = -1;
    loop->watchers[fd].events = 0;
    loop->watchers[fd].cb = NULL;
    loop->watchers[fd].data = NULL;
    /* Keep gen: zeroing makes the next publish reuse gen=1 and a queued
       epoll/kqueue event from the prior registration can match the new
       client on the recycled fd (HUP/ERR → teardown). */
    cmq_mutex_unlock(&loop->watchers_lock);
}

static int watcher_snapshot(cmq_ev_loop_t *loop, int fd, uint32_t expect_gen,
                             cmq_ev_cb_t *cb, void **data) {
    cmq_mutex_lock(&loop->watchers_lock);
    int ok = (fd >= 0 && fd < loop->watchers_cap && expect_gen != 0 &&
              loop->watchers[fd].cb &&
              loop->watchers[fd].gen == expect_gen);
    if (ok) {
        *cb = loop->watchers[fd].cb;
        *data = loop->watchers[fd].data;
    }
    cmq_mutex_unlock(&loop->watchers_lock);
    return ok;
}

static int cmq_ev_ensure_watcher(cmq_ev_loop_t *loop, int fd) {
    if (fd < loop->watchers_cap) return 0;
    /* Refuse runtime grow: pre-sized at create. Growing here from a foreign
       thread would UAF cmq_ev_run's watchers pointer. */
    return -1;
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
    if (ev_begin_op(loop) != 0) return -1;
    if (cmq_ev_ensure_watcher(loop, fd) != 0) { ev_end_op(loop); return -1; }

    /* Publish before epoll_ctl so a racing wait never sees cb==NULL. */
    uint32_t gen = watcher_publish(loop, fd, events, cb, data);

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = (uint32_t)cmq_to_epoll_events(events);
    ev.data.u64 = ev_pack_fd_gen(fd, gen);

    if (epoll_ctl(loop->backend_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        /* Stale epoll entry after a failed DEL + fd reuse. Prefer MOD so we
           do not open a DEL window where interest is gone before re-ADD. */
        if (errno != EEXIST) {
            watcher_clear(loop, fd);
            ev_end_op(loop);
            return -1;
        }
        if (epoll_ctl(loop->backend_fd, EPOLL_CTL_MOD, fd, &ev) == 0) {
            ev_end_op(loop);
            return 0;
        }
        if (errno == ENOENT &&
            epoll_ctl(loop->backend_fd, EPOLL_CTL_ADD, fd, &ev) == 0) {
            ev_end_op(loop);
            return 0;
        }
        /* Last resort: DEL then ADD (may briefly unwatch). */
        (void)epoll_ctl(loop->backend_fd, EPOLL_CTL_DEL, fd, NULL);
        if (epoll_ctl(loop->backend_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
            watcher_clear(loop, fd);
            ev_end_op(loop);
            return -1;
        }
    }
    ev_end_op(loop);
    return 0;
}

int cmq_ev_mod(cmq_ev_loop_t *loop, int fd, int events, cmq_ev_cb_t cb, void *data) {
    if (!loop || fd < 0 || fd >= loop->watchers_cap) return -1;
    if (ev_begin_op(loop) != 0) return -1;

    cmq_mutex_lock(&loop->watchers_lock);
    int was_present = (loop->watchers[fd].fd >= 0);
    int old_events = loop->watchers[fd].events;
    cmq_ev_cb_t old_cb = loop->watchers[fd].cb;
    void *old_data = loop->watchers[fd].data;
    cmq_mutex_unlock(&loop->watchers_lock);

    /* Publish before epoll_ctl so a racing wait never sees stale cb/data. */
    uint32_t gen = watcher_publish(loop, fd, events, cb, data);

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = (uint32_t)cmq_to_epoll_events(events);
    ev.data.u64 = ev_pack_fd_gen(fd, gen);

    if (epoll_ctl(loop->backend_fd, EPOLL_CTL_MOD, fd, &ev) != 0) {
        /* fd may have been dropped from the set; re-ADD once. */
        if (errno != ENOENT ||
            epoll_ctl(loop->backend_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
            /* Align kqueue: restore prior interest+cb when possible. MOD fail
               usually leaves the old epoll entry — do not clear into a silent
               dispatch hole (or mismatched new cb over old filters). */
            if (was_present && old_events != 0) {
                uint32_t rgen =
                    watcher_publish(loop, fd, old_events, old_cb, old_data);
                struct epoll_event old_ev;
                memset(&old_ev, 0, sizeof(old_ev));
                old_ev.events = (uint32_t)cmq_to_epoll_events(old_events);
                old_ev.data.u64 = ev_pack_fd_gen(fd, rgen);
                if (epoll_ctl(loop->backend_fd, EPOLL_CTL_MOD, fd, &old_ev) == 0 ||
                    (errno == ENOENT &&
                     epoll_ctl(loop->backend_fd, EPOLL_CTL_ADD, fd,
                               &old_ev) == 0)) {
                    ev_end_op(loop);
                    return -1;
                }
            }
            /* Restore failed — drop leftover interest then clear table. */
            (void)epoll_ctl(loop->backend_fd, EPOLL_CTL_DEL, fd, NULL);
            watcher_clear(loop, fd);
            ev_end_op(loop);
            return -1;
        }
    }
    ev_end_op(loop);
    return 0;
}

int cmq_ev_del(cmq_ev_loop_t *loop, int fd) {
    if (!loop || fd < 0 || fd >= loop->watchers_cap) return -1;
    if (ev_begin_op(loop) != 0) return -1;
    cmq_mutex_lock(&loop->watchers_lock);
    int present = (loop->watchers[fd].fd >= 0);
    cmq_mutex_unlock(&loop->watchers_lock);
    if (!present) {
        ev_end_op(loop);
        return -1;
    }

    /* Clear before DEL so wait cannot dispatch after teardown begins. */
    watcher_clear(loop, fd);

    if (epoll_ctl(loop->backend_fd, EPOLL_CTL_DEL, fd, NULL) != 0 &&
        errno != ENOENT) {
        ev_end_op(loop);
        return -1;
    }
    ev_end_op(loop);
    return 0;
}

int cmq_ev_run(cmq_ev_loop_t *loop, int timeout_ms) {
    if (!loop) return -1;
    if (ev_begin_op(loop) != 0) return -1;
    if (ev_run_claim(loop) != 0) {
        ev_end_op(loop);
        return 0;
    }
    ev_run_enter(loop);

    struct epoll_event events[CMQ_EV_MAX_EVENTS];

    while (cmq_atomic_load_int(&loop->running, CMQ_ATOMIC_ACQUIRE) ==
           CMQ_EV_RS_RUN) {
        int wait_ms = timeout_ms;

        uint64_t now = cmq_ev_now_ms();
        cmq_mutex_lock(&loop->timers_lock);
        for (int i = 0; i < CMQ_EV_MAX_TIMERS; i++) {
            cmq_ev_timer_t *t = &loop->timers[i];
            if (!t->active) continue;
            /* Stay in uint64_t — cast to int64_t turns UINT64_MAX / far
               expire into negative → wait_ms=0 busy-spin while dispatch
               never fires (unsigned expire_ms > now). */
            uint64_t rem = (t->expire_ms > now) ? (t->expire_ms - now) : 0;
            if (rem > (uint64_t)INT_MAX) rem = (uint64_t)INT_MAX;
            if ((int)rem < wait_ms || wait_ms < 0) wait_ms = (int)rem;
        }
        cmq_mutex_unlock(&loop->timers_lock);

        int nfds = epoll_wait(loop->backend_fd, events, CMQ_EV_MAX_EVENTS, wait_ms);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            ev_run_release(loop);
            ev_run_leave(loop);
            ev_end_op(loop);
            return -1;
        }

        for (int i = 0; i < nfds; i++) {
            int fd;
            uint32_t gen;
            ev_unpack_fd_gen(events[i].data.u64, &fd, &gen);
            if (fd == loop->wakeup_fd) {
                uint64_t val;
                read(fd, &val, sizeof(val));
                continue;
            }
            cmq_ev_cb_t cb = NULL;
            void *data = NULL;
            if (watcher_snapshot(loop, fd, gen, &cb, &data)) {
                int ev = epoll_to_cmq_events((int)events[i].events);
                cb(fd, ev, data);
            }
        }

        now = cmq_ev_now_ms();
        cmq_ev_timers_dispatch(loop, now);

        if (loop->post_tick) loop->post_tick(loop->post_tick_data);

        if (timeout_ms >= 0) break;
    }
    ev_run_release(loop);
    ev_run_leave(loop);
    ev_end_op(loop);
    return 0;
}

#elif CMQ_OS_MACOS || CMQ_OS_FREEBSD || CMQ_OS_OPENBSD || CMQ_OS_NETBSD

static int kqueue_to_cmq_events(short filter, int flags) {
    int events = 0;
    if (filter == EVFILT_READ)  events |= CMQ_EV_READ;
    if (filter == EVFILT_WRITE) events |= CMQ_EV_WRITE;
    if (flags & EV_ERROR) events |= CMQ_EV_ERROR;
    return events;
}

static int kqueue_add_filters(int kq, int fd, int events, uint32_t gen) {
    struct kevent ev[2];
    int n = 0;
    void *ud = (void *)(uintptr_t)gen;
    if (events & CMQ_EV_READ)
        EV_SET(&ev[n++], (uintptr_t)fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, ud);
    if (events & CMQ_EV_WRITE)
        EV_SET(&ev[n++], (uintptr_t)fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, ud);
    if (n == 0) return -1;
    return kevent(kq, ev, n, NULL, 0, NULL) == 0 ? 0 : -1;
}

int cmq_ev_add(cmq_ev_loop_t *loop, int fd, int events, cmq_ev_cb_t cb, void *data) {
    if (!loop || fd < 0) return -1;
    if (ev_begin_op(loop) != 0) return -1;
    if (cmq_ev_ensure_watcher(loop, fd) != 0) { ev_end_op(loop); return -1; }

    uint32_t gen = watcher_publish(loop, fd, events, cb, data);
    if (kqueue_add_filters(loop->backend_fd, fd, events, gen) != 0) {
        watcher_clear(loop, fd);
        ev_end_op(loop);
        return -1;
    }
    ev_end_op(loop);
    return 0;
}

int cmq_ev_mod(cmq_ev_loop_t *loop, int fd, int events, cmq_ev_cb_t cb, void *data) {
    if (!loop || fd < 0 || fd >= loop->watchers_cap) return -1;
    if (ev_begin_op(loop) != 0) return -1;

    cmq_mutex_lock(&loop->watchers_lock);
    int old_events = loop->watchers[fd].events;
    cmq_ev_cb_t old_cb = loop->watchers[fd].cb;
    void *old_data = loop->watchers[fd].data;
    cmq_mutex_unlock(&loop->watchers_lock);

    uint32_t gen = watcher_publish(loop, fd, events, cb, data);

    struct kevent ev[2];
    int n = 0;

    if (old_events & CMQ_EV_READ)
        EV_SET(&ev[n++], (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    if (old_events & CMQ_EV_WRITE)
        EV_SET(&ev[n++], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);

    if (n > 0) kevent(loop->backend_fd, ev, n, NULL, 0, NULL);

    if (kqueue_add_filters(loop->backend_fd, fd, events, gen) != 0) {
        /* Restore previous filters + cb/data — do not leave fd unwatched
           with a mismatched new callback (wrong client on next dispatch). */
        if (old_events != 0) {
            uint32_t rgen =
                watcher_publish(loop, fd, old_events, old_cb, old_data);
            if (kqueue_add_filters(loop->backend_fd, fd, old_events, rgen) != 0) {
                /* Align epoll: restore failed after DELETE — clear table so
                   callers see -1 as "no interest" and tear down. */
                struct kevent del[2];
                int dn = 0;
                if (old_events & CMQ_EV_READ)
                    EV_SET(&del[dn++], (uintptr_t)fd, EVFILT_READ,
                           EV_DELETE, 0, 0, NULL);
                if (old_events & CMQ_EV_WRITE)
                    EV_SET(&del[dn++], (uintptr_t)fd, EVFILT_WRITE,
                           EV_DELETE, 0, 0, NULL);
                if (dn > 0)
                    (void)kevent(loop->backend_fd, del, dn, NULL, 0, NULL);
                watcher_clear(loop, fd);
            }
        } else {
            watcher_clear(loop, fd);
        }
        ev_end_op(loop);
        return -1;
    }
    ev_end_op(loop);
    return 0;
}

int cmq_ev_del(cmq_ev_loop_t *loop, int fd) {
    if (!loop || fd < 0 || fd >= loop->watchers_cap) return -1;
    if (ev_begin_op(loop) != 0) return -1;
    cmq_mutex_lock(&loop->watchers_lock);
    int present = (loop->watchers[fd].fd >= 0);
    int old_events = loop->watchers[fd].events;
    cmq_mutex_unlock(&loop->watchers_lock);
    if (!present) {
        ev_end_op(loop);
        return -1;
    }

    watcher_clear(loop, fd);

    struct kevent ev[2];
    int n = 0;
    if (old_events & CMQ_EV_READ)
        EV_SET(&ev[n++], (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    if (old_events & CMQ_EV_WRITE)
        EV_SET(&ev[n++], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    if (n > 0 && kevent(loop->backend_fd, ev, n, NULL, 0, NULL) != 0 &&
        errno != ENOENT) {
        ev_end_op(loop);
        return -1;
    }
    ev_end_op(loop);
    return 0;
}

int cmq_ev_run(cmq_ev_loop_t *loop, int timeout_ms) {
    if (!loop) return -1;
    if (ev_begin_op(loop) != 0) return -1;
    if (ev_run_claim(loop) != 0) {
        ev_end_op(loop);
        return 0;
    }
    ev_run_enter(loop);

    struct kevent events[CMQ_EV_MAX_EVENTS];

    while (cmq_atomic_load_int(&loop->running, CMQ_ATOMIC_ACQUIRE) ==
           CMQ_EV_RS_RUN) {
        int wait_ms = timeout_ms;

        uint64_t now = cmq_ev_now_ms();
        cmq_mutex_lock(&loop->timers_lock);
        for (int i = 0; i < CMQ_EV_MAX_TIMERS; i++) {
            cmq_ev_timer_t *t = &loop->timers[i];
            if (!t->active) continue;
            /* Stay in uint64_t — cast to int64_t turns UINT64_MAX / far
               expire into negative → wait_ms=0 busy-spin while dispatch
               never fires (unsigned expire_ms > now). */
            uint64_t rem = (t->expire_ms > now) ? (t->expire_ms - now) : 0;
            if (rem > (uint64_t)INT_MAX) rem = (uint64_t)INT_MAX;
            if ((int)rem < wait_ms || wait_ms < 0) wait_ms = (int)rem;
        }
        cmq_mutex_unlock(&loop->timers_lock);

        struct timespec ts;
        struct timespec *tsp = NULL;
        if (wait_ms >= 0) {
            ts.tv_sec = wait_ms / 1000;
            ts.tv_nsec = (long)(wait_ms % 1000) * 1000000L;
            tsp = &ts;
        }

        int nfds = kevent(loop->backend_fd, NULL, 0, events, CMQ_EV_MAX_EVENTS, tsp);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            ev_run_release(loop);
            ev_run_leave(loop);
            ev_end_op(loop);
            return -1;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = (int)events[i].ident;
            uint32_t gen = (uint32_t)(uintptr_t)events[i].udata;
            if (fd == loop->wakeup_fd) {
                char buf[64];
                while (read(fd, buf, sizeof(buf)) > 0) { }
                continue;
            }
            cmq_ev_cb_t cb = NULL;
            void *data = NULL;
            if (watcher_snapshot(loop, fd, gen, &cb, &data)) {
                int ev = kqueue_to_cmq_events(events[i].filter, (int)events[i].flags);
                cb(fd, ev, data);
            }
        }

        now = cmq_ev_now_ms();
        cmq_ev_timers_dispatch(loop, now);

        if (loop->post_tick) loop->post_tick(loop->post_tick_data);

        if (timeout_ms >= 0) break;
    }
    ev_run_release(loop);
    ev_run_leave(loop);
    ev_end_op(loop);
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
    if (ev_begin_op(loop) != 0) return -1;
    cmq_mutex_lock(&loop->timers_lock);
    for (int i = 0; i < CMQ_EV_MAX_TIMERS; i++) {
        /* Skip slots still in unlocked callback (one-shot already disarmed). */
        if (!loop->timers[i].active && !loop->timers[i].firing) {
            uint64_t now = cmq_ev_now_ms();
            /* Avoid signed overflow UB; skip 0 and ids still in use after wrap. */
            int tid = -1;
            for (int tries = 0; tries < CMQ_EV_MAX_TIMERS + 2; tries++) {
                if (loop->next_timer_id <= 0 || loop->next_timer_id == INT_MAX)
                    loop->next_timer_id = 1;
                int cand = loop->next_timer_id++;
                int clash = 0;
                for (int j = 0; j < CMQ_EV_MAX_TIMERS; j++) {
                    /* Any non-zero residue blocks reuse (active/firing/tombstone). */
                    if (loop->timers[j].timer_id == cand) {
                        clash = 1;
                        break;
                    }
                }
                if (!clash) {
                    tid = cand;
                    break;
                }
            }
            if (tid < 0) {
                cmq_mutex_unlock(&loop->timers_lock);
                ev_end_op(loop);
                return -1;
            }
            loop->timers[i].timer_id = tid;
            if (delay_ms > UINT64_MAX - now)
                loop->timers[i].expire_ms = UINT64_MAX;
            else
                loop->timers[i].expire_ms = now + delay_ms;
            loop->timers[i].interval_ms = interval_ms;
            loop->timers[i].cb = cb;
            loop->timers[i].data = data;
            loop->timers[i].repeat = (interval_ms > 0) ? 1 : 0;
            loop->timers[i].firing = 0;
            loop->timers[i].cancelled = 0;
            loop->timers[i].active = 1;
            cmq_mutex_unlock(&loop->timers_lock);
            ev_end_op(loop);
            return tid;
        }
    }
    cmq_mutex_unlock(&loop->timers_lock);
    ev_end_op(loop);
    return -1;
}

int cmq_ev_timer_del(cmq_ev_loop_t *loop, int timer_id) {
    if (!loop || timer_id <= 0) return -1;
    if (ev_begin_op(loop) != 0) return -1;
    cmq_mutex_lock(&loop->timers_lock);
    for (int i = 0; i < CMQ_EV_MAX_TIMERS; i++) {
        cmq_ev_timer_t *t = &loop->timers[i];
        if (t->timer_id != timer_id)
            continue;
        /* One-shot already committed to fire (disarmed under lock). */
        if (t->firing && !t->active) {
            cmq_mutex_unlock(&loop->timers_lock);
            ev_end_op(loop);
            return -1;
        }
        /* Inactive tombstone after wrap — keep scanning for a live slot. */
        if (!t->active && !t->firing)
            continue;
        if (t->firing) {
            /* Repeat timer: suppress reschedule; wait so userdata is unused. */
            t->cancelled = 1;
            t->active = 0;
            t->repeat = 0;
            cmq_mutex_unlock(&loop->timers_lock);
            if (!ev_timer_del_reentrant(loop)) {
                for (;;) {
                    cmq_mutex_lock(&loop->timers_lock);
                    int done = !t->firing || t->timer_id != timer_id;
                    if (done) {
                        if (t->timer_id == timer_id) {
                            t->cb = NULL;
                            t->data = NULL;
                            t->cancelled = 0;
                            t->timer_id = 0;
                        }
                        cmq_mutex_unlock(&loop->timers_lock);
                        ev_end_op(loop);
                        return 0;
                    }
                    cmq_mutex_unlock(&loop->timers_lock);
                    struct timespec ts = {0, 100000L}; /* 0.1ms */
                    nanosleep(&ts, NULL);
                }
            }
            /* Reentrant del from run-thread timer cb — dispatch clears after. */
            ev_end_op(loop);
            return 0;
        }
        t->active = 0;
        t->repeat = 0;
        t->cb = NULL;
        t->data = NULL;
        t->cancelled = 0;
        t->timer_id = 0;
        cmq_mutex_unlock(&loop->timers_lock);
        ev_end_op(loop);
        return 0;
    }
    cmq_mutex_unlock(&loop->timers_lock);
    ev_end_op(loop);
    return -1;
}

void cmq_ev_stop(cmq_ev_loop_t *loop) {
    if (!loop) return;
    if (ev_begin_op(loop) != 0) return;
    /* STOP sticks until run claims/exits — never lose a pre-run stop. */
    cmq_atomic_store_int(&loop->running, CMQ_EV_RS_STOP, CMQ_ATOMIC_RELEASE);
    ev_wakeup_raw(loop);
    ev_end_op(loop);
}

void cmq_ev_wakeup(cmq_ev_loop_t *loop) {
    if (!loop) return;
    if (ev_begin_op(loop) != 0) return;
    ev_wakeup_raw(loop);
    ev_end_op(loop);
}

int cmq_ev_fd(cmq_ev_loop_t *loop) {
    if (!loop || atomic_load_explicit(&loop->dying, memory_order_acquire))
        return -1;
    return loop->backend_fd;
}

void cmq_ev_set_post_tick(cmq_ev_loop_t *loop, cmq_ev_tick_t tick, void *data) {
    if (!loop) return;
    if (ev_begin_op(loop) != 0) return;
    loop->post_tick = tick;
    loop->post_tick_data = data;
    ev_end_op(loop);
}
