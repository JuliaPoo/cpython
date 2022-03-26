/*
 * Python/bfs.c
 *
 * Simplified implementation of the Brain F**k Scheduler by Con Kolivas
 * http://ck.kolivas.org/patches/bfs/sched-BFS.txt
 *
 * Copyright (c) 2010 Nir Aides <nir@winpdb.org> and individual contributors.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of Nir Aides nor the names of other contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Simplified implementation of the Brain F**k Scheduler by Con Kolivas
 * http://ck.kolivas.org/patches/bfs/sched-BFS.txt
 *
 * "The goal of the Brain F**k Scheduler, referred to as BFS from here on, is to
 * completely do away with the complex designs of the past for the cpu process
 * scheduler and instead implement one that is very simple in basic design.
 * The main focus of BFS is to achieve excellent desktop interactivity and
 * responsiveness without heuristics and tuning knobs that are difficult to
 * understand, impossible to model and predict the effect of, and when tuned to
 * one workload cause massive detriment to another." - Con Kolivas
 *
 * Notes:
 * There are several mechanisms in this implementation which are particular to
 * Python:
 *
 * 1) Clock - Except for Linux, most platforms do not provide API to query
 * thread's running time with high precision. Therefore timing is done with
 * wall clock. The scheduler seems to behave reasonably with wall clock even
 * under load since the OS scheduler does not tend to preempt IO bound
 * threads.
 *
 * 2) Clock precision - Code works best with high precision clock 1ms--.
 * With low precision clock, scheduler will become sensitive to tasks which
 * are synchronized with the interval size. There is a related discussion
 * about this in the BFS design doc under sub-tick accounting.
 *
 * 3) TSC - When available the code will try to use TSC for timing. TSC is
 * high precision and fast to read and eliminates the timestamp reading
 * overhead. Other time sources such as HPET typically cost around 1usec per
 * call, sometimes even 3usec or more. Code is written such that it is
 * generally indifferent to multi-core counter sync and CPU-cycles/sec
 * ratio swings.
 *
 * 3) Slow Python ticks - Querying the clock between Python ticks is too
 * expensive, so it is done once every 5000 ticks. However, since even a
 * single slow Python tick can deplete a slice (10ms), a special flag exists
 * (bfs_check_depleted) which is set by waiting threads and which forces the
 * running thread to check its remaining slice by next tick.
 *
 * 4) Schedule snatching - Allow thread to snatch schedule if the signaled
 * next-to-run thread did not take over yet and is less urgent. This
 * reduces context switching, for example in case of fast yield-schedule
 * cycles, such as with IO call that ends being non-blocking.
 *
 * 5) Force switch - A thread which yields to BFS may signal the next thread
 * but continue to run (e.g. in C library code). If the OS schedules the
 * next thread on the same core, it will take a while before it actually gets
 * to run. Therefore to prevent this situation, when a thread yields to BFS,
 * it will block if the next thread has more urgent deadline, until that
 * thread starts running. This improves throughput and latency.
 *
 * 6) Multi-core clock - On older multi core CPU, dynamic frequency scaling
 * may result in unsynchronized timetamp readings between cores. The code
 * addresses this problem with by using a "Python" clock created by
 * accumulating timestamp (capped) diffs.
 *
 * 7) Disable thread boost - On Windows code disables thread boosting
 * associated with scheduler-condition for CPU bound threads. This prevents
 * them from getting higher priority, messing up scheduling across the system.
 */


#include "time.h"
#include "mutexed.h"
#include "cycle.h"


/* Round robin interval - thread time slice for running (in seconds).
 * It is set at 10ms to match Windows tick resolution. */
static const long double rr_interval = 0.010;

/* Magic number taken from Con's implementation. There it is computed as
 * 1.1 ^ 20 (nice level of regular thread). Basically it controls the load
 * beyond which threads tend to expire their deadline. Expired threads are
 * handled in FIFO order, which means that at such load IO bound threads
 * will no longer preempt running threads. */
#define DEADLINE_FACTOR 8

/* Protect access to data structures. */
static MUTEX_T bfs_mutex;

/* List all threads waiting for schedule. */
static struct _list bfs_rq;

/* Number of threads waiting for schedule */
static volatile int bfs_threads_waiting = 0;

/* Current running thread. */
static volatile PyThreadState *bfs_thread = NULL;

/* Mark current thread pointer as opaque so code does not try to access it.
 * This is relevant to shutdown. */
static int bfs_thread_opaque = 0;

/* Previous running thread. */
static PyThreadState *bfs_thread_prev = NULL;

/* Pointer to yielding thread waiting for switch confirmation. */
static volatile PyThreadState *bfs_thread_switch = NULL;

/* Time to way for urgent thread to reaquire schedule in fast
 * yield, schedule scenarios (non-blocking IO). */
#define SWITCH_BACK_TIMEOUT 0.000012

/* Use as cross-multiple-CPU-cores "clock". */
static volatile long double bfs_python_time = 0;

/* Number of TSC ticks per second. */
static long double bfs_tsc_freq = 0;
static int bfs_tsc_disabled = 0;

/* Flag currently running thread to yield immediately. */
static int bfs_preempt = 0;

/* Flag currently running thread to check remaining slice immediately. */
static int bfs_check_depleted = 0;

static int bfs_initialized = 0;

//#define VERBOSE
#undef VERBOSE
#ifdef VERBOSE
#define TRACE(v) printf v
#else
#define TRACE(v)
#endif


#ifdef MS_WINDOWS

#define DISABLE_CPU_BOUND_THREAD_BOOST(tstate) \
    if (tstate == NULL || tstate->bfs_slice <= 0 || tstate->bfs_deadline > bfs_python_time) { \
        SetThreadPriorityBoost(GetCurrentThread(), TRUE); \
    }

#define RESTORE_THREAD_BOOST() \
    SetThreadPriorityBoost(GetCurrentThread(), FALSE);

/* Return system wide timestamp in seconds (with usec precision). */
_LOCAL(long double) get_timestamp(void) {
	static LARGE_INTEGER ctrStart;
	static long double divisor = 0.0;
	LARGE_INTEGER now;

	if (divisor == 0.0) {
		LARGE_INTEGER freq;
		QueryPerformanceCounter(&ctrStart);
		QueryPerformanceFrequency(&freq);
		divisor = (long double) freq.QuadPart;
	}

	QueryPerformanceCounter(&now);
	return (now.QuadPart - ctrStart.QuadPart) / divisor;
}

#elif defined(HAVE_GETTIMEOFDAY)

#define DISABLE_CPU_BOUND_THREAD_BOOST(tstate)
#define RESTORE_THREAD_BOOST()

/* Return system wide timestamp in seconds (with usec precision). */
_LOCAL(long double) get_timestamp(void) {
    struct timeval tv;
    GETTIMEOFDAY(&tv);
    return (long double) (tv.tv_sec + tv.tv_usec * 0.000001);
}

#else

#error You need either a POSIX-compatible or a Windows system!

#endif /* MS_WINDOWS, HAVE_GETTIMEOFDAY */


#ifdef HAVE_TICK_COUNTER_LARGE_INTEGER
#define GETTICKS() ((long double)getticks().QuadPart)
#elif defined(HAVE_TICK_COUNTER)
#define GETTICKS() ((long double)getticks())
#else
#define GETTICKS() 0
#endif


/* Calibrate TSC counter frequency. */
long double bfs_tsc_calibrate0(void) {
    long double tk0, tk1, ts0, ts1;
    int i;

    tk0 = GETTICKS();
    ts0 = get_timestamp();
    do {
        for(i = 0; i < 1000; i++);
    } while (get_timestamp() - ts0 < 0.000030);
    tk1 = GETTICKS();
    ts1 = get_timestamp();
    return  (tk1 - tk0) / (ts1 - ts0);
}


/* Calibrate TSC counter frequency. */
long double bfs_tsc_calibrate1(void) {
    long double f0 = bfs_tsc_calibrate0();
    long double f1 = bfs_tsc_calibrate0();

    if (0.6 * f0 < f1 && f1 < 1.5 * f0)
        return (f0 + f1) / 2;

    return 0;
}


/* Calibrate TSC counter frequency. */
void bfs_tsc_calibrate(void) {
    if (GETTICKS() != 0) {
        bfs_tsc_freq = bfs_tsc_calibrate1();
        if (bfs_tsc_freq != 0)
            return;

        bfs_tsc_freq = bfs_tsc_calibrate1();
        if (bfs_tsc_freq != 0)
            return;
    }

    bfs_tsc_disabled = 1;
}


/* Query TSC or fall back to gettimeofday() */
_LOCAL(long double) get_cpu_timestamp(void) {
    if (bfs_tsc_disabled)
        return get_timestamp();

    return GETTICKS() / bfs_tsc_freq;
}


/* Lock bfs mutex before fork() to make sure it is not held by an uncopied
 * thread */
void bfs_fork_prepare(void) {
    MUTEX_LOCK(bfs_mutex);
}


/* Unlock mutex after fork() at parent to allow normal operation. */
void bfs_fork_parent(void) {
    MUTEX_UNLOCK(bfs_mutex);
}


/* Initialize bfs and unlock mutex after fork() at child to allow normal
 * operation. */
void bfs_fork_child(void) {
    _list_init(&bfs_rq);
    MUTEX_UNLOCK(bfs_mutex);
}


void bfs_init(void) {
#ifdef _POSIX_THREADS
    pthread_atfork(bfs_fork_prepare, bfs_fork_parent, bfs_fork_child);
#endif
    MUTEX_INIT(bfs_mutex);
    _list_init(&bfs_rq);
    bfs_tsc_calibrate();
    bfs_initialized = 1;
}


/* Pick next thread to schedule based on earliest deadline. */
static PyThreadState *bfs_find_task(long double python_time) {
    struct _list *item;
    PyThreadState *task;

    if (_list_empty(&bfs_rq))
        return NULL;

    item = bfs_rq.next;
    task = THREAD_STATE(item);

    /* Search for thread with earliest deadline or first expired deadline.
     * IO bound threads typically have expired deadlines since they naturally
     * take longer than their original deadline to deplete their slice. */
    for (item = item->next; item != &bfs_rq && task->bfs_deadline > python_time; item = item->next) {
        PyThreadState *tstate = THREAD_STATE(item);
        if (tstate->bfs_deadline < task->bfs_deadline) {
            task = tstate;
        }
    }

    return task;
}


/* Wait for currently running thread to signal this thread (tstate)
 * to take over the schedule. Call with locked mutex */
static void _bfs_timed_wait(PyThreadState *tstate, long double timestamp) {
    /* Use timeout to guard against slow ticks of running thread, but
     * multiply by number of waiting threads to prevent growing number of
     * context switches. */
    long double timeout = bfs_threads_waiting * rr_interval * 0.55;
    int timed_out = 0;

    COND_TIMED_WAIT(tstate->bfs_cond, bfs_mutex, timeout, timestamp, timed_out);

    /* Flag running thread to check slice depletion immediately. it has possibly
     * been doing slow ticks (e.g. regular expression searches) and depleted its
     * slice. */
    if (timed_out) {
        bfs_check_depleted = 1;
        struct _ceval_runtime_state *ceval = &tstate->interp->runtime->ceval;
        struct _ceval_state *ceval2 = &tstate->interp->ceval;
        COMPUTE_EVAL_BREAKER(tstate->interp, ceval, ceval2);
    }
}


/* Diff timestamp capping results to protect against clock differences
 * between cores. */
_LOCAL(long double) _bfs_diff_ts(long double ts1, long double ts0) {
    if (ts1 < ts0)
        return 0;

    if (ts1 - ts0 > rr_interval)
        return rr_interval;

    return ts1 - ts0;
}


/* Schedule (tstate) thread for running. */
void bfs_schedule(PyThreadState *tstate) {
#ifdef VERBOSE
    long double _slice = tstate->bfs_slice;
    long double _deadline = tstate->bfs_deadline;
#endif

    int is_urgent;
    volatile int spin;
    long double ts0;

    MUTEX_LOCK(bfs_mutex);

    /* Disable boost inside mutex due to tstate lifetime trickiness during
     * shutdown. */
    DISABLE_CPU_BOUND_THREAD_BOOST(tstate);

    /* Refill depleted slice and reset scheduling deadline. */
    if (tstate->bfs_slice <= 0) {
        tstate->bfs_slice = rr_interval;
        tstate->bfs_deadline = bfs_python_time + rr_interval * DEADLINE_FACTOR;
    }

    TRACE(("SCHD %p - %Lf, pt=%Lf, slice=%Lf, deadline-in=%Lf, deadline-on=%Lf\n", tstate, get_timestamp(), bfs_python_time, _slice, _deadline - bfs_python_time, tstate->bfs_deadline));

    do {
        /* Schedule immediately if no thread is currently running. */
        if (bfs_thread == NULL)
            break;

        /* Determine urgency based on deadline. If deadline expired handle
         * in FIFO order. */
        is_urgent =  !bfs_thread_opaque &&
            tstate->bfs_deadline <= bfs_thread->bfs_deadline &&
            bfs_thread->bfs_deadline >= bfs_python_time;

        /* Schedule immediately if next thread (bfs_thread) did not take
         * over yet and is less urgent. */
        if (is_urgent && bfs_thread_prev != NULL)
            break;

        struct _ceval_runtime_state *ceval = &tstate->interp->runtime->ceval;
        struct _ceval_state *ceval2 = &tstate->interp->ceval;
        /* Preempt running thread if it is less urgent. */
        if (is_urgent) {
            bfs_preempt = 1;
            COMPUTE_EVAL_BREAKER(tstate->interp, ceval, ceval2);
        }

        /* Flag running thread to check depletion of its slice. */
        else {
            bfs_check_depleted = 1;
            COMPUTE_EVAL_BREAKER(tstate->interp, ceval, ceval2);
        }

        /* Queue and wait for schedule */

        _list_append(&bfs_rq, &tstate->bfs_list);
        bfs_threads_waiting++;

        COND_RESET(tstate->bfs_cond);
        while (bfs_thread != tstate) {
            _bfs_timed_wait(tstate, 0);
            /* If yielding thread is more urgent give it a chance to
             * reschedule by spinning a little. Reduces multi-core switching
             * on fast yield-schedule cycles. */
            if (bfs_thread == tstate && bfs_thread_prev != NULL &&
                tstate->bfs_deadline > bfs_thread_prev->bfs_deadline &&
                tstate->bfs_deadline >= bfs_python_time) {
                MUTEX_UNLOCK(bfs_mutex);
                ts0 = get_timestamp();
                do {
                    for (spin = 500; spin > 0 && bfs_thread == tstate; spin--);
                } while (get_timestamp() - ts0 < SWITCH_BACK_TIMEOUT);
                MUTEX_LOCK(bfs_mutex);
            }
        }

        _list_pop(&tstate->bfs_list);
        bfs_threads_waiting--;
        break;
    } while(0);

    bfs_thread = tstate;
    bfs_thread_prev = NULL;

    /* Signal previous thread if it is waiting for the switch. */
    if (bfs_thread_switch != NULL) {
        COND_SIGNAL(((PyThreadState*)bfs_thread_switch)->bfs_cond);
        bfs_thread_switch = NULL;
    }

    tstate->bfs_timestamp = get_cpu_timestamp();

    TRACE(("TAKE %p - %Lf, pt=%Lf, tsc=%Lf\n", tstate, get_timestamp(), bfs_python_time, tstate->bfs_timestamp));
    MUTEX_UNLOCK(bfs_mutex);
    RESTORE_THREAD_BOOST();
}


/* Yield schedule to another thread. */
static void _bfs_yield(PyThreadState *tstate, int fast_reschedule) {
    long double interval;

    /* Update running time slice (book keeping). */
    if (!bfs_thread_opaque && tstate != NULL) {
        interval = _bfs_diff_ts(get_cpu_timestamp(), tstate->bfs_timestamp);
        bfs_python_time += interval;
        tstate->bfs_slice -= interval;
    }

    DISABLE_CPU_BOUND_THREAD_BOOST(tstate);
    MUTEX_LOCK(bfs_mutex);
    TRACE(("DROP %p - %Lf, pt=%Lf, tsc=%Lf\n", tstate, get_timestamp(), bfs_python_time, get_cpu_timestamp()));

    /* Reset preemption flags - we heard the shout. */
    bfs_preempt = 0;
    bfs_check_depleted = 0;
    struct _ceval_runtime_state *ceval = &tstate->interp->runtime->ceval;
    struct _ceval_state *ceval2 = &tstate->interp->ceval;
     COMPUTE_EVAL_BREAKER(tstate->interp, ceval, ceval2);

    /* Allow re-grabbing schedule back before next thread in fast
     * yield-schedule cycles. */
    if (!bfs_thread_opaque && fast_reschedule) {
        bfs_thread_prev = tstate;
    }

    /* Find earliest deadline thread to run */
    bfs_thread = bfs_find_task(bfs_python_time);
    if (bfs_thread != NULL) {
        TRACE(("SGNL %p - %Lf, signal %p\n", tstate, get_timestamp(), bfs_thread));
        COND_SIGNAL(((PyThreadState*)bfs_thread)->bfs_cond);
    }

    /* Force yield of OS slice if next thread is more urgent. This
     * is required since OS may schedule next thread to run on same core. */
    if (!bfs_thread_opaque && bfs_thread != NULL &&
        bfs_thread_switch == NULL && tstate != NULL &&
        (tstate->bfs_slice <= 0 || bfs_thread->bfs_deadline < bfs_python_time ||
        tstate->bfs_deadline >= bfs_thread->bfs_deadline)) {
        bfs_thread_switch = tstate;
        COND_RESET(tstate->bfs_cond);
        while (bfs_thread_switch == tstate) {
            COND_WAIT(tstate->bfs_cond, bfs_mutex);
        }
    }

    bfs_thread_opaque = 0;

    MUTEX_UNLOCK(bfs_mutex);
    RESTORE_THREAD_BOOST();
}


/* Yield schedule to another thread. */
static void bfs_yield(PyThreadState *tstate) {
    _bfs_yield(tstate, 1);
}


/* Check if thread depleted its running time slice. */
_LOCAL(int) bfs_depleted_slice(PyThreadState *tstate) {
    /* A reading is about 1 usec on a modern CPU. */
    return tstate->bfs_slice <= _bfs_diff_ts(get_cpu_timestamp(), tstate->bfs_timestamp);
}


/* Propose scheduler to switch to a different thread - a la cooperative
 * multitasking. */
_LOCAL(void) bfs_checkpoint(PyThreadState *tstate) {
    TRACE(("CHCK %p - %Lf, preempt=%d, check_depleted=%d\n", tstate, get_timestamp(), bfs_preempt, bfs_check_depleted));

    bfs_check_depleted = 0;
    struct _ceval_runtime_state *ceval = &tstate->interp->runtime->ceval;
    struct _ceval_state *ceval2 = &tstate->interp->ceval;
    COMPUTE_EVAL_BREAKER(tstate->interp, ceval, ceval2);

    if (bfs_preempt || bfs_depleted_slice(tstate)) {
        _bfs_yield(tstate, 0);
        bfs_schedule(tstate);
    }
}


/* Clean up, called when clearing up a Python thread state. */
void bfs_clear(PyThreadState *tstate) {
    if (bfs_initialized == 0)
        return;

    MUTEX_LOCK(bfs_mutex);

    /* If attempt to clear current thread, mark it as opaque so it is
     * not accessed by other threads attempting to schedule. */
    if (bfs_thread == tstate) {
        bfs_thread_opaque = 1;
    }
    /* If attempt to clear prev-thread, reset it to NULL so it is not accessed
     * by other threads. */
    else if (bfs_thread_prev == tstate) {
        bfs_thread_prev = NULL;
    }

    /* Remove cleared thread from waiting list. */
    if (tstate->bfs_list.next != NULL) {
        _list_pop(&tstate->bfs_list);
    }
    else {
        COND_DESTROY(tstate->bfs_cond);
    }

    MUTEX_UNLOCK(bfs_mutex);
}
