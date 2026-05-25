#ifndef CMQ_THREAD_H
#define CMQ_THREAD_H

#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include "cmq_platform.h"

typedef pthread_t cmq_thread_t;
typedef pthread_mutex_t cmq_mutex_t;
typedef pthread_cond_t cmq_cond_t;
typedef pthread_rwlock_t cmq_rwlock_t;

#if CMQ_OS_LINUX
typedef pthread_spinlock_t cmq_spinlock_t;
#endif

static cmq_inline int cmq_thread_create(cmq_thread_t *t, void *(*fn)(void *), void *arg) {
    return pthread_create(t, NULL, fn, arg);
}

static cmq_inline int cmq_thread_join(cmq_thread_t t) {
    return pthread_join(t, NULL);
}

static cmq_inline cmq_thread_t cmq_thread_self(void) {
    return pthread_self();
}

static cmq_inline int cmq_mutex_init(cmq_mutex_t *m) {
    return pthread_mutex_init(m, NULL);
}

static cmq_inline int cmq_mutex_destroy(cmq_mutex_t *m) {
    return pthread_mutex_destroy(m);
}

static cmq_inline int cmq_mutex_lock(cmq_mutex_t *m) {
    return pthread_mutex_lock(m);
}

static cmq_inline int cmq_mutex_unlock(cmq_mutex_t *m) {
    return pthread_mutex_unlock(m);
}

static cmq_inline int cmq_cond_init(cmq_cond_t *c) {
    return pthread_cond_init(c, NULL);
}

static cmq_inline int cmq_cond_destroy(cmq_cond_t *c) {
    return pthread_cond_destroy(c);
}

static cmq_inline int cmq_cond_wait(cmq_cond_t *c, cmq_mutex_t *m) {
    return pthread_cond_wait(c, m);
}

static cmq_inline int cmq_cond_signal(cmq_cond_t *c) {
    return pthread_cond_signal(c);
}

static cmq_inline int cmq_cond_broadcast(cmq_cond_t *c) {
    return pthread_cond_broadcast(c);
}

static cmq_inline int cmq_rwlock_init(cmq_rwlock_t *rw) {
    return pthread_rwlock_init(rw, NULL);
}

static cmq_inline int cmq_rwlock_destroy(cmq_rwlock_t *rw) {
    return pthread_rwlock_destroy(rw);
}

static cmq_inline int cmq_rwlock_rdlock(cmq_rwlock_t *rw) {
    return pthread_rwlock_rdlock(rw);
}

static cmq_inline int cmq_rwlock_wrlock(cmq_rwlock_t *rw) {
    return pthread_rwlock_wrlock(rw);
}

static cmq_inline int cmq_rwlock_unlock(cmq_rwlock_t *rw) {
    return pthread_rwlock_unlock(rw);
}

#endif
