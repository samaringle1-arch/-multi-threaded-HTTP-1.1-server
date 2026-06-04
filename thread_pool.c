/*
 * thread_pool.c — thread pool implementation (Week 3)
 *
 * Read thread_pool.h first for the architecture overview.
 *
 * Locking discipline (critical — follow this every time you touch
 * the pool to avoid deadlocks):
 *
 *   RULE 1: Always acquire pool->mutex before reading or writing
 *           pool->queue, pool->shutdown, or pool->active_workers.
 *
 *   RULE 2: Always release pool->mutex before calling handle_client().
 *           Holding the lock across a blocking I/O call would
 *           serialise all workers back to single-threaded behaviour.
 *
 *   RULE 3: Use while(), not if(), to re-check conditions after
 *           pthread_cond_wait() returns. Spurious wakeups are real.
 *
 *   RULE 4: Signal/broadcast after changing state the other side
 *           is waiting on. Forgetting a signal causes a thread to
 *           sleep forever (a "lost wakeup").
 */

#include "thread_pool.h"
#include "server.h"        /* handle_client(), logging macros */
#include "http_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

/* ------------------------------------------------------------------ */
/*  Internal queue helpers (called with mutex held)                    */
/* ------------------------------------------------------------------ */

static int queue_is_empty(const WorkQueue *q)
{
    return q->count == 0;
}

static int queue_is_full(const WorkQueue *q)
{
    return q->count == QUEUE_CAPACITY;
}

static void queue_push(WorkQueue *q, WorkItem item)
{
    q->items[q->tail] = item;
    q->tail           = (q->tail + 1) % QUEUE_CAPACITY;  /* wrap around */
    q->count++;
}

static WorkItem queue_pop(WorkQueue *q)
{
    WorkItem item = q->items[q->head];
    q->head       = (q->head + 1) % QUEUE_CAPACITY;       /* wrap around */
    q->count--;
    return item;
}

/* ------------------------------------------------------------------ */
/*  worker_thread                                                      */
/*                                                                     */
/*  Each worker runs this loop forever until shutdown.                */
/*                                                                     */
/*  Pattern: lock → wait-if-empty → pop → unlock → do work → repeat  */
/* ------------------------------------------------------------------ */
void *worker_thread(void *arg)
{
    ThreadPool *pool = (ThreadPool *)arg;

    char thread_id[32];
    snprintf(thread_id, sizeof(thread_id), "%lu",
             (unsigned long)pthread_self());

    printf("[worker %s] started\n", thread_id);

    for (;;) {
        /* ---------------------------------------------------------- */
        /* Phase 1: Wait for work (or shutdown signal)                */
        /* ---------------------------------------------------------- */
        pthread_mutex_lock(&pool->mutex);

        /*
         * Use WHILE — not IF — because:
         *   a) pthread_cond_wait() can return spuriously (POSIX allows it)
         *   b) Another worker may have stolen the item between our
         *      wakeup and our re-acquisition of the mutex
         *
         * This is called the "mesa-style" condition variable pattern.
         */
        while (queue_is_empty(&pool->queue) && !pool->shutdown) {
            pthread_cond_wait(&pool->not_empty, &pool->mutex);
        }

        /*
         * Shutdown path: if we're shutting down AND the queue is
         * drained, this worker's job is done. Exit cleanly.
         *
         * Note: we keep working while shutdown=1 but queue is non-empty
         * — this is the "graceful drain" that ensures in-flight clients
         * always receive a response.
         */
        if (pool->shutdown && queue_is_empty(&pool->queue)) {
            printf("[worker %s] queue drained — exiting\n", thread_id);
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        /* ---------------------------------------------------------- */
        /* Phase 2: Dequeue one work item                             */
        /* ---------------------------------------------------------- */
        WorkItem item = queue_pop(&pool->queue);

        /*
         * Signal the producer (main thread) that a slot opened up.
         * If the main thread was blocked in thread_pool_submit()
         * because the queue was full, this wakes it up.
         */
        pthread_cond_signal(&pool->not_full);

        pthread_mutex_unlock(&pool->mutex);

        /* ---------------------------------------------------------- */
        /* Phase 3: Handle the client — NO lock held during I/O       */
        /* ---------------------------------------------------------- */
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &item.client_addr.sin_addr,
                  client_ip, sizeof(client_ip));
        printf("[worker %s] handling %s:%d  (fd=%d)\n",
               thread_id, client_ip,
               ntohs(item.client_addr.sin_port), item.client_fd);

        // AFTER — handle_connection() already closes the fd
handle_client(item.client_fd);
/* fd is already closed inside handle_connection() */
printf("[worker %s] finished fd=%d\n", thread_id, item.client_fd);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  thread_pool_init                                                   */
/*                                                                     */
/*  Initialise all synchronisation primitives, then launch workers.   */
/* ------------------------------------------------------------------ */
void thread_pool_init(ThreadPool *pool)
{
    /* Zero everything first — catches uninitialised-field bugs */
    memset(pool, 0, sizeof(*pool));

    /* ---- Mutex --------------------------------------------------- */
    if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
        perror("[pool] pthread_mutex_init");
        exit(EXIT_FAILURE);
    }

    /* ---- Condition variables ------------------------------------- */
    if (pthread_cond_init(&pool->not_empty, NULL) != 0) {
        perror("[pool] pthread_cond_init not_empty");
        exit(EXIT_FAILURE);
    }
    if (pthread_cond_init(&pool->not_full, NULL) != 0) {
        perror("[pool] pthread_cond_init not_full");
        exit(EXIT_FAILURE);
    }

    /* ---- Launch worker threads ----------------------------------- */
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0) {
            perror("[pool] pthread_create");
            exit(EXIT_FAILURE);
        }
        pool->active_workers++;
    }

    printf("[pool] started %d worker threads  (queue capacity: %d)\n",
           THREAD_POOL_SIZE, QUEUE_CAPACITY);
}

/* ------------------------------------------------------------------ */
/*  thread_pool_submit                                                 */
/*                                                                     */
/*  Called by the main thread after accept().                         */
/*  Blocks if the queue is full (backpressure — don't drop clients).  */
/* ------------------------------------------------------------------ */
void thread_pool_submit(ThreadPool *pool, int client_fd,
                        struct sockaddr_in client_addr)
{
    pthread_mutex_lock(&pool->mutex);

    /*
     * If the queue is full, block until a worker pops an item.
     * This provides backpressure: the main thread slows down naturally
     * rather than filling an unbounded queue and running out of memory.
     *
     * Also bail if shutdown was requested while we were waiting —
     * no point queuing new work we won't process.
     */
    while (queue_is_full(&pool->queue) && !pool->shutdown) {
        printf("[pool] queue full — main thread blocking (backpressure)\n");
        pthread_cond_wait(&pool->not_full, &pool->mutex);
    }

    if (pool->shutdown) {
        /* Server is shutting down — reject the new connection */
        pthread_mutex_unlock(&pool->mutex);
        close(client_fd);
        return;
    }

    WorkItem item = { .client_fd   = client_fd,
                      .client_addr = client_addr };
    queue_push(&pool->queue, item);

    /*
     * Signal ONE waiting worker that there's something to do.
     * Using signal() (not broadcast()) to avoid the "thundering herd":
     * waking all N threads for 1 item means N-1 threads find nothing
     * and go back to sleep — wasted context switches.
     */
    pthread_cond_signal(&pool->not_empty);

    pthread_mutex_unlock(&pool->mutex);
}

/* ------------------------------------------------------------------ */
/*  thread_pool_destroy                                                */
/*                                                                     */
/*  Graceful shutdown:                                                 */
/*    1. Set shutdown flag                                             */
/*    2. Wake all workers (they'll drain the queue then exit)         */
/*    3. Join every thread (wait for them to finish)                  */
/* ------------------------------------------------------------------ */
void thread_pool_destroy(ThreadPool *pool)
{
    printf("[pool] shutdown requested — draining queue (%d items)...\n",
           pool->queue.count);

    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;

    /*
     * Broadcast (not signal) here — we need ALL workers to wake up
     * and eventually see the shutdown flag, not just one.
     */
    pthread_cond_broadcast(&pool->not_empty);
    pthread_cond_broadcast(&pool->not_full);   /* unblock main if stuck */

    pthread_mutex_unlock(&pool->mutex);

    /* Wait for every worker to finish its current client and exit */
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_join(pool->threads[i], NULL);
        printf("[pool] worker %d joined\n", i);
    }

    /* Clean up primitives */
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->not_empty);
    pthread_cond_destroy(&pool->not_full);

    printf("[pool] all workers exited cleanly\n");
}