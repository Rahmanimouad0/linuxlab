/*
 * race_lab - a data race, measured, and four ways to (not) fix it.
 *
 *   Thread A ─┐
 *             ├── counter++   (load, add, store: three steps, not one)
 *   Thread B ─┘
 *
 * Strategies:
 *   racy    shared counter, no synchronization          -> lost updates
 *   mutex   pthread_mutex around every increment        -> correct, slow
 *   atomic  atomic_fetch_add (one locked instruction)   -> correct, faster
 *   local   private counter per thread, merged at end   -> correct, fastest
 *
 * usage: race_lab [-t threads] [-n increments-per-thread] [-r trials] [--check]
 *   --check  exit 1 if any *synchronized* strategy loses an update (for tests)
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

typedef enum { RACY, MUTEX, ATOMIC, LOCAL } strategy;
static const char *names[] = {"racy", "mutex", "atomic", "local"};

static long iterations;
/* volatile forces a real load and store on every ++ in the racy version;
 * otherwise the optimizer would keep the counter in a register and add the
 * total once, which hides the interleaving we want to observe. */
static volatile long racy_counter;
static long mutex_counter;
static atomic_long atomic_counter;
static long local_total;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg)
{
    strategy s = *(strategy *)arg;
    switch (s) {
    case RACY:
        for (long i = 0; i < iterations; i++)
            racy_counter++;
        break;
    case MUTEX:
        for (long i = 0; i < iterations; i++) {
            pthread_mutex_lock(&lock);
            mutex_counter++;
            pthread_mutex_unlock(&lock);
        }
        break;
    case ATOMIC:
        for (long i = 0; i < iterations; i++)
            atomic_fetch_add_explicit(&atomic_counter, 1, memory_order_relaxed);
        break;
    case LOCAL: {
        volatile long mine = 0; /* volatile: keep the loop honest */
        for (long i = 0; i < iterations; i++)
            mine++;
        pthread_mutex_lock(&lock);
        local_total += mine;
        pthread_mutex_unlock(&lock);
        break;
    }
    }
    return NULL;
}

static long run(strategy s, int threads, double *ms)
{
    racy_counter = mutex_counter = local_total = 0;
    atomic_store(&atomic_counter, 0);

    pthread_t *tids = xcalloc((size_t)threads, sizeof *tids);
    uint64_t t0 = now_ns();
    for (int i = 0; i < threads; i++)
        if (pthread_create(&tids[i], NULL, worker, &s) != 0)
            die("pthread_create failed");
    for (int i = 0; i < threads; i++)
        pthread_join(tids[i], NULL);
    *ms = (double)(now_ns() - t0) / 1e6;
    free(tids);

    switch (s) {
    case RACY: return racy_counter;
    case MUTEX: return mutex_counter;
    case ATOMIC: return atomic_load(&atomic_counter);
    case LOCAL: return local_total;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int threads = 4, trials = 5, check = 0;
    iterations = 1000000;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
            threads = atoi(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            iterations = atol(argv[++i]);
        else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc)
            trials = atoi(argv[++i]);
        else if (strcmp(argv[i], "--check") == 0)
            check = 1;
        else
            die("usage: race_lab [-t threads] [-n increments] [-r trials] [--check]");
    }
    if (threads < 1 || iterations < 1 || trials < 1)
        die("arguments must be positive");

    long expected = (long)threads * iterations;
    printf("%d threads x %ld increments = %ld expected, %d trials each\n\n", threads, iterations,
           expected, trials);
    printf("%-8s %14s %14s %9s %10s\n", "strategy", "worst result", "lost updates", "lost %",
           "median ms");

    int failed = 0;
    for (strategy s = RACY; s <= LOCAL; s++) {
        long worst = expected;
        double *times = xcalloc((size_t)trials, sizeof *times);
        for (int t = 0; t < trials; t++) {
            long got = run(s, threads, &times[t]);
            if (got < worst)
                worst = got;
        }
        /* median of the trial times (insertion sort, trials is small) */
        for (int a = 1; a < trials; a++)
            for (int b = a; b > 0 && times[b - 1] > times[b]; b--) {
                double tmp = times[b];
                times[b] = times[b - 1];
                times[b - 1] = tmp;
            }
        long lost = expected - worst;
        printf("%-8s %14ld %14ld %8.2f%% %10.2f\n", names[s], worst, lost,
               100.0 * (double)lost / (double)expected, times[trials / 2]);
        if (s != RACY && lost != 0)
            failed = 1;
        free(times);
    }

    printf("\nThe racy counter loses updates whenever two threads load the same old\n"
           "value and both store old+1. On a single CPU core losses are rare; on\n"
           "several cores they are almost guaranteed.\n");
    return check && failed ? 1 : 0;
}
