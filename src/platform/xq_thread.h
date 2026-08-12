/* SPDX-License-Identifier: Apache-2.0 */
#ifndef XQ_THREAD_H
#define XQ_THREAD_H

#include <stddef.h>
#include "xq.h"

#if defined(_WIN32)
typedef void *xq_thread_handle;
typedef struct { void *cs; } xq_mutex;
typedef struct { void *cv; } xq_cond;
#else
#include <pthread.h>
typedef pthread_t       xq_thread_handle;
typedef pthread_mutex_t xq_mutex;
typedef pthread_cond_t  xq_cond;
#endif

typedef struct {
    xq_thread_handle h;
    int              started;
} xq_thread;

int xq_cpu_count(void);

xq_status xq_mutex_init(xq_mutex *m);
void      xq_mutex_destroy(xq_mutex *m);
void      xq_mutex_lock(xq_mutex *m);
void      xq_mutex_unlock(xq_mutex *m);

xq_status xq_cond_init(xq_cond *c);
void      xq_cond_destroy(xq_cond *c);
void      xq_cond_wait(xq_cond *c, xq_mutex *m);
void      xq_cond_signal(xq_cond *c);
void      xq_cond_broadcast(xq_cond *c);

xq_status xq_thread_start(xq_thread *t, void *(*fn)(void *), void *arg);
void      xq_thread_join(xq_thread *t);

#endif
