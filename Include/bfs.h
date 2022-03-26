/*
 * Python/bfs.h
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
 */


#ifndef Py_BFS_H
#define Py_BFS_H
#ifdef __cplusplus
extern "C" {
#endif


extern void bfs_clear(PyThreadState *tstate);
extern void bfs_init(void);
extern void bfs_schedule(PyThreadState *tstate);

#ifdef __cplusplus
}
#endif
#endif /* !Py_BFS_H */
