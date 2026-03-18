#ifndef PLATFORM_THREAD_H
#define PLATFORM_THREAD_H

/*
 * platform_thread.h — thin cross-platform threading abstraction.
 *
 * POSIX path  (!_WIN32): pass-through wrappers around pthreads.
 * Win32 path  (_WIN32) : CRITICAL_SECTION / CONDITION_VARIABLE / HANDLE.
 *
 * Requires <stdlib.h> to be included before this header (for free() used
 * in the Win32 trampoline).  That is already satisfied by every consumer.
 */

#ifndef _WIN32
/* ------------------------------------------------------------------ */
/*  POSIX implementation                                               */
/* ------------------------------------------------------------------ */

#include <pthread.h>
#include <unistd.h>

typedef pthread_t       PT_Thread;
typedef pthread_mutex_t PT_Mutex;
typedef pthread_cond_t  PT_Cond;

static inline void pt_mutex_init(PT_Mutex* m)    { pthread_mutex_init(m, NULL); }
static inline void pt_mutex_lock(PT_Mutex* m)    { pthread_mutex_lock(m); }
static inline void pt_mutex_unlock(PT_Mutex* m)  { pthread_mutex_unlock(m); }
static inline void pt_mutex_destroy(PT_Mutex* m) { pthread_mutex_destroy(m); }

static inline void pt_cond_init(PT_Cond* c)                         { pthread_cond_init(c, NULL); }
static inline void pt_cond_wait(PT_Cond* c, PT_Mutex* m)            { pthread_cond_wait(c, m); }
static inline void pt_cond_signal(PT_Cond* c)                       { pthread_cond_signal(c); }
static inline void pt_cond_broadcast(PT_Cond* c)                    { pthread_cond_broadcast(c); }
static inline void pt_cond_destroy(PT_Cond* c)                      { pthread_cond_destroy(c); }

static inline void pt_thread_create(PT_Thread* t, void*(*fn)(void*), void* arg)
{
    pthread_create(t, NULL, fn, arg);
}
static inline void pt_thread_join(PT_Thread t) { pthread_join(t, NULL); }

static inline int pt_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int)n : 1;
}

#else /* _WIN32 */
/* ------------------------------------------------------------------ */
/*  Win32 implementation                                               */
/* ------------------------------------------------------------------ */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef HANDLE             PT_Thread;
typedef CRITICAL_SECTION   PT_Mutex;
typedef CONDITION_VARIABLE PT_Cond;

static inline void pt_mutex_init(PT_Mutex* m)    { InitializeCriticalSection(m); }
static inline void pt_mutex_lock(PT_Mutex* m)    { EnterCriticalSection(m); }
static inline void pt_mutex_unlock(PT_Mutex* m)  { LeaveCriticalSection(m); }
static inline void pt_mutex_destroy(PT_Mutex* m) { DeleteCriticalSection(m); }

static inline void pt_cond_init(PT_Cond* c)      { InitializeConditionVariable(c); }
static inline void pt_cond_wait(PT_Cond* c, PT_Mutex* m)
{
    SleepConditionVariableCS(c, m, INFINITE);
}
static inline void pt_cond_signal(PT_Cond* c)    { WakeConditionVariable(c); }
static inline void pt_cond_broadcast(PT_Cond* c) { WakeAllConditionVariable(c); }
static inline void pt_cond_destroy(PT_Cond* c)   { (void)c; /* no-op on Win32 */ }

/* Trampoline: bridges CreateThread's LPTHREAD_START_ROUTINE to void*(void*). */
typedef struct { void*(*fn)(void*); void* arg; } PT__ThreadArgs;

static DWORD WINAPI pt__trampoline(LPVOID param)
{
    PT__ThreadArgs* a = (PT__ThreadArgs*)param;
    void*(*fn)(void*) = a->fn;
    void* arg         = a->arg;
    free(a);
    fn(arg);
    return 0;
}

static inline void pt_thread_create(PT_Thread* t, void*(*fn)(void*), void* arg)
{
    PT__ThreadArgs* a = (PT__ThreadArgs*)malloc(sizeof(PT__ThreadArgs));
    a->fn  = fn;
    a->arg = arg;
    *t = CreateThread(NULL, 0, pt__trampoline, a, 0, NULL);
}

static inline void pt_thread_join(PT_Thread t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

static inline int pt_cpu_count(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

#endif /* _WIN32 */
#endif /* PLATFORM_THREAD_H */
