/*
 * bench_ipc - latency and throughput of pipes, Unix sockets and shared memory.
 *
 * Latency: ping-pong of a 64-byte message between two processes; reported
 * as the round-trip time.
 *   pipe          two pipes, one per direction
 *   unix socket   one socketpair(AF_UNIX, SOCK_STREAM)
 *   shm+semaphore shared memory, process-shared POSIX semaphores (futex)
 *   shm+spin      shared memory, busy-waiting on an atomic flag (no syscalls)
 *
 * Throughput: one process streams N bytes to the other in chunks of 4 KiB,
 * 64 KiB and 1 MiB.
 *   pipe, unix socket     write()/read(): the kernel copies twice
 *   shm (double buffer)   memcpy in, memcpy out, semaphores to hand over
 */
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bench.h"
#include "util.h"

#define MSG 64

typedef enum { PIPE, SOCKET } fd_kind;

static void write_all(int fd, const char *buf, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w <= 0)
            die_errno("write");
        buf += w;
        n -= (size_t)w;
    }
}

static void read_all(int fd, char *buf, size_t n)
{
    while (n > 0) {
        ssize_t r = read(fd, buf, n);
        if (r <= 0)
            die_errno("read");
        buf += r;
        n -= (size_t)r;
    }
}

/* Set up a bidirectional channel: parent uses p_in/p_out, child c_in/c_out. */
static void make_channel(fd_kind k, int *p_in, int *p_out, int *c_in, int *c_out)
{
    if (k == PIPE) {
        int to_child[2], to_parent[2];
        if (pipe(to_child) < 0 || pipe(to_parent) < 0)
            die_errno("pipe");
        *p_out = to_child[1];
        *c_in = to_child[0];
        *c_out = to_parent[1];
        *p_in = to_parent[0];
    } else {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
            die_errno("socketpair");
        *p_in = *p_out = sv[0];
        *c_in = *c_out = sv[1];
    }
}

static void close_channel(fd_kind k, int a, int b)
{
    close(a);
    if (k == PIPE)
        close(b);
}

/* ------------------------------------------------------------ latency */

static double latency_fd(fd_kind k, int iters)
{
    int p_in, p_out, c_in, c_out;
    make_channel(k, &p_in, &p_out, &c_in, &c_out);
    char msg[MSG] = {0};

    pid_t pid = fork();
    if (pid == 0) {
        close_channel(k, p_in, p_out);
        for (int i = 0; i < iters + 1; i++) {
            read_all(c_in, msg, MSG);
            write_all(c_out, msg, MSG);
        }
        _exit(0);
    }
    close_channel(k, c_in, c_out);

    write_all(p_out, msg, MSG); /* warm-up round trip */
    read_all(p_in, msg, MSG);
    uint64_t t0 = now_ns();
    for (int i = 0; i < iters; i++) {
        write_all(p_out, msg, MSG);
        read_all(p_in, msg, MSG);
    }
    uint64_t t1 = now_ns();
    waitpid(pid, NULL, 0);
    close_channel(k, p_in, p_out);
    return (double)(t1 - t0) / 1e3 / iters;
}

struct pingpong {
    sem_t ping, pong;
    atomic_int turn; /* for the spin variant: 1 = child's turn */
    char msg[MSG];
};

static double latency_shm(int spin, int iters)
{
    struct pingpong *pp =
        mmap(NULL, sizeof *pp, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (pp == MAP_FAILED)
        die_errno("mmap");
    sem_init(&pp->ping, 1, 0);
    sem_init(&pp->pong, 1, 0);
    atomic_store(&pp->turn, 0);
    char local[MSG] = {0};

    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 0; i < iters + 1; i++) {
            if (spin) {
                while (atomic_load_explicit(&pp->turn, memory_order_acquire) != 1) {
                }
                memcpy(local, pp->msg, MSG);
                atomic_store_explicit(&pp->turn, 0, memory_order_release);
            } else {
                sem_wait(&pp->ping);
                memcpy(local, pp->msg, MSG);
                sem_post(&pp->pong);
            }
        }
        _exit(0);
    }

    uint64_t t0 = 0;
    for (int i = 0; i < iters + 1; i++) {
        if (i == 1)
            t0 = now_ns(); /* round 0 is the warm-up */
        memcpy(pp->msg, local, MSG);
        if (spin) {
            atomic_store_explicit(&pp->turn, 1, memory_order_release);
            while (atomic_load_explicit(&pp->turn, memory_order_acquire) != 0) {
            }
        } else {
            sem_post(&pp->ping);
            sem_wait(&pp->pong);
        }
    }
    uint64_t t1 = now_ns();
    waitpid(pid, NULL, 0);
    sem_destroy(&pp->ping);
    sem_destroy(&pp->pong);
    munmap(pp, sizeof *pp);
    return (double)(t1 - t0) / 1e3 / iters;
}

/* --------------------------------------------------------- throughput */

static double throughput_fd(fd_kind k, size_t chunk, size_t total)
{
    int p_in, p_out, c_in, c_out;
    make_channel(k, &p_in, &p_out, &c_in, &c_out);
    char *buf = xmalloc(chunk);
    memset(buf, 'x', chunk);

    uint64_t t0 = now_ns();
    pid_t pid = fork();
    if (pid == 0) { /* the child is the writer */
        close_channel(k, p_in, p_out);
        for (size_t sent = 0; sent < total; sent += chunk)
            write_all(c_out, buf, chunk);
        _exit(0);
    }
    close_channel(k, c_in, c_out);
    for (size_t got = 0; got < total; got += chunk)
        read_all(p_in, buf, chunk);
    uint64_t t1 = now_ns();
    waitpid(pid, NULL, 0);
    close_channel(k, p_in, p_out);
    free(buf);
    return (double)total / (1 << 20) / ((double)(t1 - t0) / 1e9);
}

struct ring {
    sem_t empty, full; /* two slots: the writer fills one while the reader drains the other */
    char data[];
};

static double throughput_shm(size_t chunk, size_t total)
{
    size_t size = sizeof(struct ring) + 2 * chunk;
    struct ring *r = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (r == MAP_FAILED)
        die_errno("mmap");
    sem_init(&r->empty, 1, 2);
    sem_init(&r->full, 1, 0);
    char *buf = xmalloc(chunk);
    memset(buf, 'x', chunk);
    memset(r->data, 0, 2 * chunk); /* pre-fault the shared pages */

    uint64_t t0 = now_ns();
    pid_t pid = fork();
    if (pid == 0) {
        size_t n = 0;
        for (size_t sent = 0; sent < total; sent += chunk, n++) {
            sem_wait(&r->empty);
            memcpy(r->data + (n % 2) * chunk, buf, chunk);
            sem_post(&r->full);
        }
        _exit(0);
    }
    size_t n = 0;
    for (size_t got = 0; got < total; got += chunk, n++) {
        sem_wait(&r->full);
        memcpy(buf, r->data + (n % 2) * chunk, chunk);
        sem_post(&r->empty);
    }
    uint64_t t1 = now_ns();
    escape(buf);
    waitpid(pid, NULL, 0);
    free(buf);
    munmap(r, size);
    return (double)total / (1 << 20) / ((double)(t1 - t0) / 1e9);
}

int main(int argc, char **argv)
{
    bench_parse_args(argc, argv);
    const int reps = bench_quick ? 3 : 7;
    const int iters = bench_quick ? 2000 : 50000;
    const size_t total = bench_quick ? (8u << 20) : (512u << 20);
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    double t[16] = {0};

    bench_header("IPC round-trip latency, 64-byte message (lower is better)");
    for (int r = 0; r < reps; r++)
        t[r] = latency_fd(PIPE, iters);
    report("ipc_latency", "pipe", "64B", median(t, reps), "us");
    for (int r = 0; r < reps; r++)
        t[r] = latency_fd(SOCKET, iters);
    report("ipc_latency", "unix socket", "64B", median(t, reps), "us");
    for (int r = 0; r < reps; r++)
        t[r] = latency_shm(0, iters);
    report("ipc_latency", "shm+semaphore", "64B", median(t, reps), "us");
    if (cpus >= 2) { /* spinning on one CPU would just burn time slices */
        for (int r = 0; r < reps; r++)
            t[r] = latency_shm(1, iters);
        report("ipc_latency", "shm+spin", "64B", median(t, reps), "us");
    }

    bench_header("IPC throughput (higher is better)");
    const size_t chunks[] = {4096, 65536, 1 << 20};
    const char *chunk_names[] = {"4KiB", "64KiB", "1MiB"};
    for (size_t c = 0; c < ARRAY_LEN(chunks); c++) {
        for (int r = 0; r < reps; r++)
            t[r] = throughput_fd(PIPE, chunks[c], total);
        report("ipc_throughput", "pipe", chunk_names[c], median(t, reps), "MiB/s");
        for (int r = 0; r < reps; r++)
            t[r] = throughput_fd(SOCKET, chunks[c], total);
        report("ipc_throughput", "unix socket", chunk_names[c], median(t, reps), "MiB/s");
        for (int r = 0; r < reps; r++)
            t[r] = throughput_shm(chunks[c], total);
        report("ipc_throughput", "shm double buffer", chunk_names[c], median(t, reps), "MiB/s");
    }
    return 0;
}
