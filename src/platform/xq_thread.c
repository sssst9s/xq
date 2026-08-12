/* SPDX-License-Identifier: Apache-2.0 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#  if defined(__APPLE__)
#    define _DARWIN_C_SOURCE
#  endif
#endif

#include <stdlib.h>
#include <string.h>

#include "xq_thread.h"

#if defined(_WIN32)

#include <windows.h>
#include <process.h>

int xq_cpu_count(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors;
    return n > 0 ? n : 1;
}

xq_status xq_mutex_init(xq_mutex *m)
{
    CRITICAL_SECTION *cs = malloc(sizeof *cs);
    if (!cs) return XQ_ERR_OOM;
    InitializeCriticalSection(cs);
    m->cs = cs;
    return XQ_OK;
}
void xq_mutex_destroy(xq_mutex *m)
{
    if (!m || !m->cs) return;
    DeleteCriticalSection((CRITICAL_SECTION *)m->cs);
    free(m->cs);
    m->cs = NULL;
}
void xq_mutex_lock(xq_mutex *m)   { EnterCriticalSection((CRITICAL_SECTION *)m->cs); }
void xq_mutex_unlock(xq_mutex *m) { LeaveCriticalSection((CRITICAL_SECTION *)m->cs); }

xq_status xq_cond_init(xq_cond *c)
{
    CONDITION_VARIABLE *cv = malloc(sizeof *cv);
    if (!cv) return XQ_ERR_OOM;
    InitializeConditionVariable(cv);
    c->cv = cv;
    return XQ_OK;
}
void xq_cond_destroy(xq_cond *c) { if (c && c->cv) { free(c->cv); c->cv = NULL; } }
void xq_cond_wait(xq_cond *c, xq_mutex *m)
{
    SleepConditionVariableCS((CONDITION_VARIABLE *)c->cv, (CRITICAL_SECTION *)m->cs, INFINITE);
}
void xq_cond_signal(xq_cond *c)    { WakeConditionVariable((CONDITION_VARIABLE *)c->cv); }
void xq_cond_broadcast(xq_cond *c) { WakeAllConditionVariable((CONDITION_VARIABLE *)c->cv); }

typedef struct { void *(*fn)(void *); void *arg; } win_trampoline;

static unsigned __stdcall win_start(void *p)
{
    win_trampoline t = *(win_trampoline *)p;
    free(p);
    t.fn(t.arg);
    return 0;
}

xq_status xq_thread_start(xq_thread *t, void *(*fn)(void *), void *arg)
{
    win_trampoline *tr = malloc(sizeof *tr);
    if (!tr) return XQ_ERR_OOM;
    tr->fn = fn; tr->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, win_start, tr, 0, NULL);
    if (!h) { free(tr); return XQ_ERR_INTERNAL; }
    t->h = (void *)h;
    t->started = 1;
    return XQ_OK;
}

void xq_thread_join(xq_thread *t)
{
    if (!t || !t->started) return;
    WaitForSingleObject((HANDLE)t->h, INFINITE);
    CloseHandle((HANDLE)t->h);
    t->started = 0;
}

#else

#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#  include <sys/sysctl.h>
#endif

int xq_cpu_count(void)
{
#if defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) return (int)n;
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
    {
        int mib[2] = { CTL_HW, HW_NCPU };
        int ncpu = 0;
        size_t len = sizeof ncpu;
        if (sysctl(mib, 2, &ncpu, &len, NULL, 0) == 0 && ncpu > 0) return ncpu;
    }
#endif
    return 1;
}

xq_status xq_mutex_init(xq_mutex *m)
{
    return pthread_mutex_init(m, NULL) == 0 ? XQ_OK : XQ_ERR_INTERNAL;
}
void xq_mutex_destroy(xq_mutex *m) { pthread_mutex_destroy(m); }
void xq_mutex_lock(xq_mutex *m)    { pthread_mutex_lock(m); }
void xq_mutex_unlock(xq_mutex *m)  { pthread_mutex_unlock(m); }

xq_status xq_cond_init(xq_cond *c)
{
    return pthread_cond_init(c, NULL) == 0 ? XQ_OK : XQ_ERR_INTERNAL;
}
void xq_cond_destroy(xq_cond *c)           { pthread_cond_destroy(c); }
void xq_cond_wait(xq_cond *c, xq_mutex *m) { pthread_cond_wait(c, m); }
void xq_cond_signal(xq_cond *c)            { pthread_cond_signal(c); }
void xq_cond_broadcast(xq_cond *c)         { pthread_cond_broadcast(c); }

xq_status xq_thread_start(xq_thread *t, void *(*fn)(void *), void *arg)
{
    if (pthread_create(&t->h, NULL, fn, arg) != 0) return XQ_ERR_INTERNAL;
    t->started = 1;
    return XQ_OK;
}

void xq_thread_join(xq_thread *t)
{
    if (!t || !t->started) return;
    pthread_join(t->h, NULL);
    t->started = 0;
}

#endif
