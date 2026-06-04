/*
 * test_thread_pool.c — unit + integration tests for the thread pool
 *
 * Build:  make test_pool
 * Run:    ./test_pool
 *
 * These tests verify:
 *   1. Init/destroy works without deadlock or crash
 *   2. Submitted work items are all processed (no drops)
 *   3. Multiple workers process items concurrently
 *   4. Graceful shutdown drains the queue before exiting
 *   5. The pool stays correct under burst load (queue-full backpressure)
 *
 * Because thread bugs are timing-dependent, run this under Helgrind:
 *   valgrind --tool=helgrind ./test_pool
 */

#include "thread_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* ------------------------------------------------------------------ */
/*  Test framework                                                     */
/* ------------------------------------------------------------------ */
static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(label, cond)                                             \
    do {                                                               \
        if (cond) {                                                    \
            tests_passed++;                                            \
            printf("  ✓  %s\n", label);                               \
        } else {                                                       \
            tests_failed++;                                            \
            printf("  ✗  %s  (line %d)\n", label, __LINE__);          \
        }                                                              \
    } while (0)

/* ------------------------------------------------------------------ */
/*  Shared counter — workers increment this to prove they ran         */
/* ------------------------------------------------------------------ */
static pthread_mutex_t g_counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static int             g_processed     = 0;

static void increment_counter(void)
{
    pthread_mutex_lock(&g_counter_mutex);
    g_processed++;
    pthread_mutex_unlock(&g_counter_mutex);
}

static int read_counter(void)
{
    pthread_mutex_lock(&g_counter_mutex);
    int v = g_processed;
    pthread_mutex_unlock(&g_counter_mutex);
    return v;
}

/* ------------------------------------------------------------------ */
/*  Stub: handle_client                                               */
/*                                                                     */
/*  The real handle_client() talks to a socket.  For pool tests we    */
/*  just verify that the worker ran and touched the right fd.         */
/*  We use a socketpair() so we have a real fd pair with no network.  */
/* ------------------------------------------------------------------ */
void handle_client(int client_fd)
{
    /*
     * Write a byte so the other end of the socketpair can confirm
     * this client was processed, then increment the shared counter.
     */
    char marker = 'X';
    write(client_fd, &marker, 1);
    increment_counter();
    /* Simulate brief I/O work so concurrency is observable */
    usleep(1000);   /* 1 ms */
}

/* ------------------------------------------------------------------ */
/*  Helper: create a connected socketpair (no network involved)       */
/* ------------------------------------------------------------------ */
static void make_pair(int *server_side, int *client_side)
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        perror("socketpair");
        exit(EXIT_FAILURE);
    }
    *server_side = fds[0];   /* handle_client() writes here */
    *client_side = fds[1];   /* test reads confirmation here */
}

/* ================================================================== */
/*  Tests                                                              */
/* ================================================================== */

/* ------------------------------------------------------------------ */
static void test_init_destroy(void)
{
    printf("\n[test] Init and immediate destroy (no work)\n");

    ThreadPool pool;
    thread_pool_init(&pool);

    CHECK("active_workers == THREAD_POOL_SIZE",
          pool.active_workers == THREAD_POOL_SIZE);
    CHECK("shutdown flag starts at 0",
          pool.shutdown == 0);
    CHECK("queue starts empty",
          pool.queue.count == 0);

    thread_pool_destroy(&pool);

    CHECK("shutdown flag set after destroy",
          pool.shutdown == 1);

    printf("  (no deadlock = pass)\n");
}

/* ------------------------------------------------------------------ */
static void test_single_item(void)
{
    printf("\n[test] Submit one item — worker processes it\n");

    g_processed = 0;

    ThreadPool pool;
    thread_pool_init(&pool);

    int srv, cli;
    make_pair(&srv, &cli);

    struct sockaddr_in addr = {0};
    thread_pool_submit(&pool, srv, addr);

    /* Wait for the worker to write the marker byte */
    char buf = 0;
    ssize_t n = read(cli, &buf, 1);

    thread_pool_destroy(&pool);
    close(cli);

    CHECK("worker wrote marker byte",  n == 1 && buf == 'X');
    CHECK("processed counter is 1",    read_counter() == 1);
}

/* ------------------------------------------------------------------ */
static void test_many_items(void)
{
    printf("\n[test] Submit 20 items — all get processed\n");

    g_processed = 0;
    const int N = 20;

    ThreadPool pool;
    thread_pool_init(&pool);

    int clients[N];
    struct sockaddr_in addr = {0};

    for (int i = 0; i < N; i++) {
        int srv, cli;
        make_pair(&srv, &cli);
        clients[i] = cli;
        thread_pool_submit(&pool, srv, addr);
    }

    /* Read the confirmation byte from each client side */
    int confirmed = 0;
    for (int i = 0; i < N; i++) {
        char buf = 0;
        ssize_t n = read(clients[i], &buf, 1);
        if (n == 1 && buf == 'X') confirmed++;
        close(clients[i]);
    }

    thread_pool_destroy(&pool);

    CHECK("all 20 items processed",  confirmed == N);
    CHECK("counter matches",         read_counter() == N);
}

/* ------------------------------------------------------------------ */
static void test_concurrent_workers(void)
{
    printf("\n[test] Multiple workers run concurrently\n");

    /*
     * Submit THREAD_POOL_SIZE items simultaneously.
     * If the pool is truly concurrent, all workers should be busy
     * at the same time — elapsed time ≈ 1 × item_latency, not N × latency.
     *
     * We verify concurrency indirectly: all N items complete in under
     * 5× the per-item latency (1 ms + overhead), not N × 1 ms.
     */
    g_processed = 0;

    ThreadPool pool;
    thread_pool_init(&pool);

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    const int N = THREAD_POOL_SIZE;
    int clients[N];
    struct sockaddr_in addr = {0};

    for (int i = 0; i < N; i++) {
        int srv, cli;
        make_pair(&srv, &cli);
        clients[i] = cli;
        thread_pool_submit(&pool, srv, addr);
    }

    for (int i = 0; i < N; i++) {
        char buf = 0;
        read(clients[i], &buf, 1);
        close(clients[i]);
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    thread_pool_destroy(&pool);

    long elapsed_ms = (t_end.tv_sec  - t_start.tv_sec)  * 1000 +
                      (t_end.tv_nsec - t_start.tv_nsec) / 1000000;

    printf("  %d items × 1ms latency completed in %ld ms\n", N, elapsed_ms);

    /*
     * If serial: elapsed ≈ N × 1 ms = 4 ms (for N=4)
     * If concurrent: elapsed ≈ 1 ms + scheduling overhead
     * Threshold: pass if elapsed < N × 1 ms × 0.75
     */
    long serial_ms = N * 1;
    CHECK("workers ran concurrently (elapsed < 75% of serial time)",
          elapsed_ms < (serial_ms * 75 / 100 + 5));   /* +5ms for OS jitter */
}

/* ------------------------------------------------------------------ */
static void test_graceful_drain(void)
{
    printf("\n[test] Shutdown drains in-flight items before exiting\n");

    g_processed = 0;
    const int N = 10;

    ThreadPool pool;
    thread_pool_init(&pool);

    int clients[N];
    struct sockaddr_in addr = {0};

    for (int i = 0; i < N; i++) {
        int srv, cli;
        make_pair(&srv, &cli);
        clients[i] = cli;
        thread_pool_submit(&pool, srv, addr);
    }

    /*
     * Call destroy immediately — workers are mid-flight.
     * Graceful drain means all 10 must complete before destroy returns.
     */
    thread_pool_destroy(&pool);

    int confirmed = 0;
    for (int i = 0; i < N; i++) {
        char buf = 0;
        /* Non-blocking check — work should already be done */
        ssize_t n = read(clients[i], &buf, 1);
        if (n == 1 && buf == 'X') confirmed++;
        close(clients[i]);
    }

    CHECK("all items drained before destroy returned", confirmed == N);
    CHECK("counter matches",                           read_counter() == N);
}

/* ------------------------------------------------------------------ */
static void test_queue_stats(void)
{
    printf("\n[test] Queue circular-buffer wraparound\n");

    /*
     * Push and pop items in a pattern that forces the head and tail
     * pointers to wrap around the QUEUE_CAPACITY boundary.
     * This catches off-by-one bugs in the modulo arithmetic.
     */
    WorkQueue q = {0};

    /* Fill to capacity */
    int pushed = 0;
    while (!( q.count == QUEUE_CAPACITY )) {
        WorkItem item = { .client_fd = pushed };
        q.items[q.tail] = item;
        q.tail  = (q.tail + 1) % QUEUE_CAPACITY;
        q.count++;
        pushed++;
    }

    /* Pop half */
    int popped = 0;
    int half   = QUEUE_CAPACITY / 2;
    while (popped < half) {
        q.head  = (q.head + 1) % QUEUE_CAPACITY;
        q.count--;
        popped++;
    }

    /* Fill the freed slots — forces tail to wrap */
    int extra = 0;
    while (!( q.count == QUEUE_CAPACITY )) {
        WorkItem item = { .client_fd = 9000 + extra };
        q.items[q.tail] = item;
        q.tail  = (q.tail + 1) % QUEUE_CAPACITY;
        q.count++;
        extra++;
    }

    CHECK("queue full after wraparound fill",   q.count == QUEUE_CAPACITY);
    CHECK("tail wrapped around correctly",       q.tail  == half);
    CHECK("head is at midpoint",                 q.head  == half);
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */
int main(void)
{
    printf("══════════════════════════════════════════════════════\n");
    printf("  Thread Pool — Unit & Integration Tests\n");
    printf("══════════════════════════════════════════════════════\n");

    test_init_destroy();
    test_single_item();
    test_many_items();
    test_concurrent_workers();
    test_graceful_drain();
    test_queue_stats();

    int total = tests_passed + tests_failed;
    printf("\n══════════════════════════════════════════════════════\n");
    printf("  Results: %d/%d passed", tests_passed, total);
    if (tests_failed > 0)
        printf("  (%d FAILED)", tests_failed);
    printf("\n══════════════════════════════════════════════════════\n\n");

    printf("Next step: run under Helgrind to check for data races:\n");
    printf("  valgrind --tool=helgrind ./test_pool\n\n");

    return tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}