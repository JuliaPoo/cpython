/*
 * Mutex related macros for the BFS scheduler - taken from ceval_gil.h.
 */

#include <stdlib.h>

#ifndef _POSIX_THREADS
/* This means pthreads are not implemented in libc headers, hence the macro
   not present in unistd.h. But they still can be implemented as an external
   library (e.g. gnu pth in pthread emulation) */
# ifdef HAVE_PTHREAD_H
#  include <pthread.h> /* _POSIX_THREADS */
# endif
#endif

#ifdef _POSIX_THREADS

/*
 * POSIX support
 */

#include <pthread.h>

#define ADD_MICROSECONDS(tv, interval) \
do { \
    tv.tv_usec += (long) interval; \
    tv.tv_sec += tv.tv_usec / 1000000; \
    tv.tv_usec %= 1000000; \
} while (0)

/* We assume all modern POSIX systems have gettimeofday() */
#ifdef GETTIMEOFDAY_NO_TZ
#define GETTIMEOFDAY(ptv) gettimeofday(ptv)
#else
#define GETTIMEOFDAY(ptv) gettimeofday(ptv, (struct timezone *)NULL)
#endif

#define MUTEX_T pthread_mutex_t
#define MUTEX_INIT(mut) \
    if (pthread_mutex_init(&mut, NULL)) { \
        Py_FatalError("pthread_mutex_init(" #mut ") failed"); };
#define MUTEX_LOCK(mut) \
    if (pthread_mutex_lock(&mut)) { \
        Py_FatalError("pthread_mutex_lock(" #mut ") failed"); };
#define MUTEX_UNLOCK(mut) \
    if (pthread_mutex_unlock(&mut)) { \
        Py_FatalError("pthread_mutex_unlock(" #mut ") failed"); };

#define COND_T pthread_cond_t
#define COND_INIT(cond) \
    if (pthread_cond_init(&cond, NULL)) { \
        Py_FatalError("pthread_cond_init(" #cond ") failed"); };
#define COND_DESTROY(cond) \
    if (pthread_cond_destroy(&cond)) { \
        Py_FatalError("pthread_cond_destroy(" #cond ") failed"); };
#define COND_RESET(cond)
#define COND_SIGNAL(cond) \
    if (pthread_cond_signal(&cond)) { \
        Py_FatalError("pthread_cond_signal(" #cond ") failed"); };
#define COND_WAIT(cond, mut) \
    if (pthread_cond_wait(&cond, &mut)) { \
        Py_FatalError("pthread_cond_wait(" #cond ") failed"); };
#define COND_TIMED_WAIT(cond, mut, seconds, timestamp_now, timeout_result) \
    { \
        int r; \
        struct timespec ts; \
        struct timeval deadline; \
        \
        if (timestamp_now == 0) { \
            GETTIMEOFDAY(&deadline); \
        } \
        else { \
            deadline.tv_sec = (int) timestamp_now; \
            deadline.tv_usec = (int) ((timestamp_now - deadline.tv_sec) * 1000000); \
        } \
        ADD_MICROSECONDS(deadline, ((int)(seconds * 1000000))); \
        ts.tv_sec = deadline.tv_sec; \
        ts.tv_nsec = deadline.tv_usec * 1000; \
        \
        r = pthread_cond_timedwait(&cond, &mut, &ts); \
        if (r == ETIMEDOUT) \
            timeout_result = 1; \
        else if (r) \
            Py_FatalError("pthread_cond_timedwait(" #cond ") failed"); \
        else \
            timeout_result = 0; \
    } \

#elif defined(NT_THREADS)

/*
 * Windows (2000 and later, as well as (hopefully) CE) support
 */

#include <windows.h>

/* Use critical section since it is significantly faster than a mutex object.
 * It should be enough for the needs of the BFS scheduler since on any
 * signaled event there is exactly one waiting thread. However the macros
 * are not suited for the general case so the code should probably move
 * somewhere else. */

#define MUTEX_T CRITICAL_SECTION
#define MUTEX_INIT(mut) \
    InitializeCriticalSectionAndSpinCount(&mut, 4000);
#define MUTEX_LOCK(mut) \
    EnterCriticalSection(&mut);
#define MUTEX_UNLOCK(mut) \
    LeaveCriticalSection(&mut);

#undef COND_T
#define COND_T HANDLE
#define COND_INIT(cond) \
    /* auto-reset, non-signalled */ \
    if (!(cond = CreateEvent(NULL, FALSE, FALSE, NULL))) { \
        Py_FatalError("CreateMutex(" #cond ") failed"); };
#define COND_DESTROY(mut) \
    if (!CloseHandle(mut)) { \
        Py_FatalError("CloseHandle(" #mut ") failed"); };
#define COND_RESET(cond) \
    if (!ResetEvent(cond)) { \
        Py_FatalError("ResetEvent(" #cond ") failed"); };
#define COND_SIGNAL(cond) \
    if (!SetEvent(cond)) { \
        Py_FatalError("SetEvent(" #cond ") failed"); };
#define COND_WAIT(cond, mut) \
    { \
        MUTEX_UNLOCK(mut); \
        if (WaitForSingleObject(cond, INFINITE) != WAIT_OBJECT_0) \
            Py_FatalError("WaitForSingleObject(" #cond ") failed"); \
        MUTEX_LOCK(mut); \
    }
#define COND_TIMED_WAIT(cond, mut, seconds, timestamp_now, timeout_result) \
    { \
        DWORD r; \
        MUTEX_UNLOCK(mut); \
        r = WaitForSingleObject(cond, (int)(seconds * 1000)); \
        if (r == WAIT_TIMEOUT) { \
            MUTEX_LOCK(mut); \
            timeout_result = 1; \
        } \
        else if (r != WAIT_OBJECT_0) \
            Py_FatalError("WaitForSingleObject(" #cond ") failed"); \
        else { \
            MUTEX_LOCK(mut); \
            timeout_result = 0; \
        } \
    }

#else

#error You need either a POSIX-compatible or a Windows system!

#endif /* _POSIX_THREADS, NT_THREADS */
