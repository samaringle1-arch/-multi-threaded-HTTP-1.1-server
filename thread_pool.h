/*
 * thread_pool.h — fixed-size thread pool with work queue (Week 3)
 *
 * Architecture: classic producer-consumer pattern.
 *
 *   main thread (producer)          worker threads (consumers)
 *   ──────────────────────          ──────────────────────────
 *   accept() new client fd    →     dequeue fd from work queue
 *   thread_pool_submit(fd)    →     handle_client(fd)
 *   loop                      →     close(fd), wait for next item
 *
 * Synchronisation primitives used:
 *
 *   mutex      — protects every read/write of the queue and flags
 *   not_empty  — workers block here when queue is empty
 *   not_full   — main thread blocks here when queue is full (backpressure)
 *
 * Shutdown sequence (graceful drain):
 *
 *   1. Main sets pool->shutdown = 1, broadcasts both condvars
 *   2. Workers finish their current client, then check:
 *        if (shutdown && queue empty) → exit
 *   3. Main calls pthread_join() on each worker
 *   4. In-flight requests complete. No connections dropped.
 */

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>
#include <netinet/in.h>

/* ------------------------------------------------------------------ */
/*  Configuration                                                      */
/* ------------------------------------------------------------------ */

/*
 * Number of worker threads.
 * Rule of thumb for I/O-bound work: number of CPU cores.
 * For CPU-bound work you'd match cores exactly; we're I/O-bound
 * (waiting on recv/send) so slightly more than core count is fine.
 *
 * Experiment in Week 6: benchmark THREAD_POOL_SIZE = 1, 2, 4, 8, 16
 * and plot req/s vs thread count. The curve flattens once threads
 * outnumber cores — that's your optimal point.
 */
#define THREAD_POOL_SIZE  4

/*
 * Maximum number of client fds that can queue up waiting for
 * a free worker. If the queue fills, the main thread blocks
 * (backpressure) rather than dropping connections.
 *
 * This is separate from the kernel's listen() backlog (BACKLOG in
 * server.h). The kernel holds unaccepted TCP connections; this queue
 * holds accepted fds waiting for a worker thread.
 */
#define QUEUE_CAPACITY    128

/* ------------------------------------------------------------------ */
/*  Work item                                                          */
/*  One entry in the queue — everything a worker needs for one client. */
/* ------------------------------------------------------------------ */
typedef struct {
    int                client_fd;    /* accepted socket fd             */
    struct sockaddr_in client_addr;  /* client IP + port (for logging) */
} WorkItem;

/* ------------------------------------------------------------------ */
/*  WorkQueue — circular buffer                                        */
/*                                                                     */
/*  A ring buffer is ideal here: O(1) enqueue and dequeue, no         */
/*  heap allocation, cache-friendly.                                   */
/*                                                                     */
/*   head → next item to dequeue (consumer reads here)                */
/*   tail → next empty slot     (producer writes here)                */
/*   count → items currently in the buffer                            */
/*                                                                     */
/*   Empty: count == 0                                                 */
/*   Full:  count == QUEUE_CAPACITY                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    WorkItem items[QUEUE_CAPACITY];
    int      head;
    int      tail;
    int      count;
} WorkQueue;

/* ------------------------------------------------------------------ */
/*  ThreadPool                                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    pthread_t       threads[THREAD_POOL_SIZE];  /* worker thread handles */
    WorkQueue       queue;                       /* shared work queue     */
    pthread_mutex_t mutex;                       /* protects queue+flags  */
    pthread_cond_t  not_empty;                   /* workers wait here     */
    pthread_cond_t  not_full;                    /* main waits here       */
    int             shutdown;                    /* 1 = begin drain+exit  */
    int             active_workers;              /* count of live threads  */
} ThreadPool;

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* Allocate, initialise, and launch all worker threads. */
void thread_pool_init(ThreadPool *pool);

/*
 * Submit a new client connection to the pool.
 * Blocks if the queue is full (backpressure) until a slot opens.
 * Safe to call only from the main/accept thread.
 */
void thread_pool_submit(ThreadPool *pool, int client_fd,
                        struct sockaddr_in client_addr);

/*
 * Signal all workers to finish their current client and exit.
 * Joins every thread before returning — blocks until fully drained.
 * Call once, after the accept loop exits.
 */
void thread_pool_destroy(ThreadPool *pool);

/* ------------------------------------------------------------------ */
/*  Internal — worker entry point                                      */
/*  Exposed in the header so it can be referenced in thread_pool.c    */
/* ------------------------------------------------------------------ */
void *worker_thread(void *arg);

#endif /* THREAD_POOL_H */