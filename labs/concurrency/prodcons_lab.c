/*
 * prodcons_lab - the bounded-buffer (producer/consumer) problem with a
 * mutex and two condition variables.
 *
 *   producers ──put()──▶ ┌───┬───┬───┬───┐ ──get()──▶ consumers
 *                        │   │ x │ x │   │   ring buffer of N slots
 *                        └───┴───┴───┴───┘
 *   put() waits while the buffer is full  (cond: not_full)
 *   get() waits while the buffer is empty (cond: not_empty)
 *
 * The program verifies that every item is consumed exactly once.
 *
 * usage: prodcons_lab [-p producers] [-c consumers] [-n items-per-producer] [-b slots]
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

typedef struct {
    long *slots;
    int size, head, tail, count;
    int producers_done;
    pthread_mutex_t lock;
    pthread_cond_t not_full, not_empty;
    long waits_full, waits_empty; /* how often a thread had to sleep */
} buffer;

static buffer buf;
static long items_per_producer;
static unsigned char *seen; /* seen[item] counts deliveries of each item */

static void put(long item)
{
    pthread_mutex_lock(&buf.lock);
    /* while, not if: after waking up the condition may be false again
     * (another producer got there first, or a spurious wake-up). */
    while (buf.count == buf.size) {
        buf.waits_full++;
        pthread_cond_wait(&buf.not_full, &buf.lock);
    }
    buf.slots[buf.tail] = item;
    buf.tail = (buf.tail + 1) % buf.size;
    buf.count++;
    pthread_cond_signal(&buf.not_empty);
    pthread_mutex_unlock(&buf.lock);
}

/* Returns 0 and stores an item, or -1 when everything has been consumed. */
static int get(long *item)
{
    pthread_mutex_lock(&buf.lock);
    while (buf.count == 0 && !buf.producers_done) {
        buf.waits_empty++;
        pthread_cond_wait(&buf.not_empty, &buf.lock);
    }
    if (buf.count == 0) { /* empty and no producer left */
        pthread_mutex_unlock(&buf.lock);
        return -1;
    }
    *item = buf.slots[buf.head];
    buf.head = (buf.head + 1) % buf.size;
    buf.count--;
    pthread_cond_signal(&buf.not_full);
    pthread_mutex_unlock(&buf.lock);
    return 0;
}

typedef struct {
    int id;
    long consumed;
    long sum;
} consumer_stats;

static void *producer(void *arg)
{
    long id = (long)(intptr_t)arg;
    for (long i = 0; i < items_per_producer; i++)
        put(id * items_per_producer + i);
    return NULL;
}

static void *consumer(void *arg)
{
    consumer_stats *st = arg;
    long item;
    while (get(&item) == 0) {
        st->consumed++;
        st->sum += item;
        seen[item]++; /* each item index is only touched by its one consumer */
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int np = 2, nc = 2, slots = 8;
    items_per_producer = 50000;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            np = atoi(argv[++i]);
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            nc = atoi(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            items_per_producer = atol(argv[++i]);
        else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
            slots = atoi(argv[++i]);
        else
            die("usage: prodcons_lab [-p producers] [-c consumers] [-n items] [-b slots]");
    }
    if (np < 1 || nc < 1 || slots < 1 || items_per_producer < 1)
        die("arguments must be positive");

    long total = (long)np * items_per_producer;
    buf.slots = xcalloc((size_t)slots, sizeof *buf.slots);
    buf.size = slots;
    seen = xcalloc((size_t)total, 1);
    pthread_mutex_init(&buf.lock, NULL);
    pthread_cond_init(&buf.not_full, NULL);
    pthread_cond_init(&buf.not_empty, NULL);

    printf("%d producers, %d consumers, %d-slot buffer, %ld items total\n", np, nc, slots, total);

    pthread_t *pt = xcalloc((size_t)np, sizeof *pt);
    pthread_t *ct = xcalloc((size_t)nc, sizeof *ct);
    consumer_stats *cs = xcalloc((size_t)nc, sizeof *cs);

    uint64_t t0 = now_ns();
    for (int i = 0; i < nc; i++) {
        cs[i].id = i;
        pthread_create(&ct[i], NULL, consumer, &cs[i]);
    }
    for (long i = 0; i < np; i++)
        pthread_create(&pt[i], NULL, producer, (void *)(intptr_t)i);
    for (int i = 0; i < np; i++)
        pthread_join(pt[i], NULL);

    /* Tell consumers no more items will come, and wake all of them. */
    pthread_mutex_lock(&buf.lock);
    buf.producers_done = 1;
    pthread_cond_broadcast(&buf.not_empty);
    pthread_mutex_unlock(&buf.lock);
    for (int i = 0; i < nc; i++)
        pthread_join(ct[i], NULL);
    double ms = (double)(now_ns() - t0) / 1e6;

    long consumed = 0, sum = 0, missing = 0, duplicates = 0;
    for (int i = 0; i < nc; i++) {
        consumed += cs[i].consumed;
        sum += cs[i].sum;
        printf("  consumer %d took %ld items\n", i, cs[i].consumed);
    }
    for (long i = 0; i < total; i++) {
        if (seen[i] == 0)
            missing++;
        else if (seen[i] > 1)
            duplicates++;
    }
    long expected_sum = total * (total - 1) / 2;

    printf("\nproduced   : %ld\n", total);
    printf("consumed   : %ld\n", consumed);
    printf("missing    : %ld\n", missing);
    printf("duplicates : %ld\n", duplicates);
    printf("checksum   : %s (%ld)\n", sum == expected_sum ? "OK" : "MISMATCH", sum);
    printf("waits      : producers slept %ld times (full), consumers %ld times (empty)\n",
           buf.waits_full, buf.waits_empty);
    printf("time       : %.1f ms (%.2f M items/s)\n", ms, (double)total / ms / 1000.0);

    int ok = consumed == total && missing == 0 && duplicates == 0 && sum == expected_sum;
    free(pt);
    free(ct);
    free(cs);
    free(seen);
    free(buf.slots);
    return ok ? 0 : 1;
}
