/*
 * bench_sync - cost of synchronization as threads are added.
 *
 * Every thread performs the same number of increments; we report the
 * aggregate throughput (million increments per second, higher is better).
 *
 *   mutex          one shared counter behind a pthread_mutex
 *   spinlock       one shared counter behind a pthread_spinlock
 *   atomic         one shared atomic counter (lock xadd)
 *   false sharing  a private counter per thread, but all in ONE cache line
 *   padded         a private counter per thread, each in its own cache line
 *   local          a private counter on each thread's stack
 *
 * "false sharing" vs "padded" isolates the cost of cache-line ping-pong:
 * the threads never touch the same variable, only the same 64-byte line.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>

#include "bench.h"
#include "util.h"

#define MAX_THREADS 16

typedef enum { MUTEX, SPIN, ATOMIC, FALSE_SHARING, PADDED, LOCAL } variant;
static const char *names[] = {"mutex", "spinlock", "atomic", "false sharing", "padded", "local"};

typedef struct {
    _Alignas(64) volatile uint64_t value;
} padded_counter;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_spinlock_t spin;
static uint64_t shared_counter;
static atomic_uint_fast64_t atomic_counter;
static volatile uint64_t packed[MAX_THREADS] __attribute__((aligned(64)));
static padded_counter padded[MAX_THREADS];
static pthread_barrier_t start;
static long ops;

typedef struct {
    int id;
    variant v;
} job;

static void *worker(void *arg)
{
    job *j = arg;
    pthread_barrier_wait(&start);
    switch (j->v) {
    case MUTEX:
        for (long i = 0; i < ops; i++) {
            pthread_mutex_lock(&mutex);
            shared_counter++;
            pthread_mutex_unlock(&mutex);
        }
        break;
    case SPIN:
        for (long i = 0; i < ops; i++) {
            pthread_spin_lock(&spin);
            shared_counter++;
            pthread_spin_unlock(&spin);
        }
        break;
    case ATOMIC:
        for (long i = 0; i < ops; i++)
            atomic_fetch_add_explicit(&atomic_counter, 1, memory_order_relaxed);
        break;
    case FALSE_SHARING:
        for (long i = 0; i < ops; i++)
            packed[j->id]++;
        break;
    case PADDED:
        for (long i = 0; i < ops; i++)
            padded[j->id].value++;
        break;
    case LOCAL: {
        volatile uint64_t mine = 0;
        for (long i = 0; i < ops; i++)
            mine++;
        break;
    }
    }
    return NULL;
}

/*
 * Uncontended variants are ~50x faster than lock-based ones; give them more
 * work so that every run lasts well over the ~100 us of thread start-up.
 */
static long ops_for(variant v, long base) { return v >= FALSE_SHARING ? base * 25 : base; }

static double run(variant v, int threads)
{
    pthread_t tid[MAX_THREADS];
    job jobs[MAX_THREADS];
    pthread_barrier_init(&start, NULL, (unsigned)threads + 1);
    for (int i = 0; i < threads; i++) {
        jobs[i].id = i;
        jobs[i].v = v;
        pthread_create(&tid[i], NULL, worker, &jobs[i]);
    }
    pthread_barrier_wait(&start); /* all threads are created and waiting */
    uint64_t t0 = now_ns();
    for (int i = 0; i < threads; i++)
        pthread_join(tid[i], NULL);
    uint64_t t1 = now_ns();
    pthread_barrier_destroy(&start);
    return (double)threads * (double)ops / ((double)(t1 - t0) / 1e9) / 1e6;
}

int main(int argc, char **argv)
{
    bench_parse_args(argc, argv);
    const int reps = bench_quick ? 3 : 5;
    const long base_ops = bench_quick ? 50000 : 2000000;
    pthread_spin_init(&spin, PTHREAD_PROCESS_PRIVATE);
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    const int thread_counts[] = {1, 2, 4, 8};

    if (!bench_csv)
        printf("(%ld CPUs online; thread counts above that are oversubscribed)\n", cpus);
    bench_header("synchronization throughput (M increments/s, higher is better)");
    for (variant v = MUTEX; v <= LOCAL; v++) {
        ops = ops_for(v, base_ops);
        for (size_t k = 0; k < ARRAY_LEN(thread_counts); k++) {
            double t[16] = {0};
            run(v, thread_counts[k]); /* warm-up */
            for (int r = 0; r < reps; r++)
                t[r] = run(v, thread_counts[k]);
            char param[32];
            snprintf(param, sizeof param, "threads=%d", thread_counts[k]);
            report("sync", names[v], param, median(t, reps), "Mops/s");
        }
    }
    pthread_spin_destroy(&spin);
    return 0;
}
