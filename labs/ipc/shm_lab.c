/*
 * shm_lab - POSIX shared memory: two processes, one set of physical pages.
 *
 *   process A                 physical memory               process B
 *   0x7f..a000 ──┐          ┌──────────────────┐        ┌── 0x7f..e000
 *                └────────▶ │  struct shared    │ ◀──────┘
 *                           └──────────────────┘
 *                         /dev/shm/linuxlab-shm-<pid>
 *
 *   1. shm_open + ftruncate + mmap, message hand-off with semaphores
 *   2. the same object can live at different virtual addresses
 *   3. shared memory has no built-in synchronization: race vs. mutex
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

#define INCREMENTS 1000000

struct shared {
    sem_t msg_ready; /* child -> parent: message written  */
    sem_t reply_ready;
    char msg[128];
    pthread_mutex_t lock; /* process-shared mutex */
    volatile long unsafe_counter;
    long safe_counter;
};

static struct shared *map_object(const char *name, int create)
{
    int fd = shm_open(name, O_RDWR | (create ? O_CREAT | O_EXCL : 0), 0600);
    if (fd < 0)
        die_errno("shm_open");
    if (create && ftruncate(fd, sizeof(struct shared)) < 0)
        die_errno("ftruncate");
    struct shared *s = mmap(NULL, sizeof(struct shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (s == MAP_FAILED)
        die_errno("mmap");
    close(fd); /* the mapping keeps the object alive */
    return s;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    char name[64];
    snprintf(name, sizeof name, "/linuxlab-shm-%d", (int)getpid());

    struct shared *s = map_object(name, 1);
    sem_init(&s->msg_ready, 1, 0); /* pshared = 1: usable across processes */
    sem_init(&s->reply_ready, 1, 0);
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&s->lock, &ma);

    section("1. message hand-off");
    printf("  object %s is visible as /dev/shm%s (%zu bytes)\n", name, name, sizeof *s);

    pid_t pid = fork();
    if (pid < 0)
        die_errno("fork");
    if (pid == 0) {
        /* Map the object a *second* time: same pages, new virtual address. */
        struct shared *mine = map_object(name, 0);
        snprintf(mine->msg, sizeof mine->msg, "hello from pid %d", (int)getpid());
        sem_post(&mine->msg_ready);
        sem_wait(&mine->reply_ready);
        printf("  child : parent replied \"%s\"\n", mine->msg);
        printf("  child : my own mapping is at %p (inherited one at %p)\n", (void *)mine,
               (void *)s);

        for (int i = 0; i < INCREMENTS; i++) {
            mine->unsafe_counter++;
            pthread_mutex_lock(&mine->lock);
            mine->safe_counter++;
            pthread_mutex_unlock(&mine->lock);
        }
        _exit(0);
    }

    sem_wait(&s->msg_ready);
    printf("  parent: read \"%s\" directly from memory - no copy, no syscall\n", s->msg);
    snprintf(s->msg, sizeof s->msg, "got it");
    sem_post(&s->reply_ready);

    for (int i = 0; i < INCREMENTS; i++) {
        s->unsafe_counter++;
        pthread_mutex_lock(&s->lock);
        s->safe_counter++;
        pthread_mutex_unlock(&s->lock);
    }
    waitpid(pid, NULL, 0);

    section("2. addresses");
    printf("  parent mapping at %p. Two mappings of one object can have different\n"
           "  addresses, so shared structures must store offsets, not pointers.\n",
           (void *)s);

    section("3. synchronization is your job");
    printf("  expected           : %d\n", 2 * INCREMENTS);
    printf("  unsynchronized ++  : %ld\n", s->unsafe_counter);
    printf("  with shared mutex  : %ld\n", s->safe_counter);

    pthread_mutex_destroy(&s->lock);
    sem_destroy(&s->msg_ready);
    sem_destroy(&s->reply_ready);
    munmap(s, sizeof *s);
    shm_unlink(name);
    printf("shared memory is the fastest IPC (no kernel copy) but the programs must\n"
           "bring their own locking; see benchmarks/bench_ipc for numbers.\n");
    return 0;
}
