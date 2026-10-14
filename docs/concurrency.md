# Threads and synchronisation

Labs: [`race_lab`](../labs/concurrency/race_lab.c), [`prodcons_lab`](../labs/concurrency/prodcons_lab.c).
Scaling measurements: [`bench_sync`](../benchmarks/bench_sync.c) → [experiments.md §4](experiments.md#4-synchronization-contention-not-locking-is-what-costs).

## A data race, measured

`counter++` looks like one operation but compiles to three: load, add, store. Two threads can
both load the same old value and both store `old + 1`, which loses one increment.

```text
Thread A:  load 41 ─────────── add → 42 ── store 42
Thread B:        load 41 ── add → 42 ─────────── store 42      two ++, one effect
```

```text
$ race_lab            (4 threads × 1,000,000 increments, worst of 5 trials)
strategy   worst result   lost updates    lost %  median ms
racy            1123696        2876304    71.91%       2.74
mutex           4000000              0     0.00%      63.85
atomic          4000000              0     0.00%      34.56
local           4000000              0     0.00%       0.71
```

* **racy** lost **72 %** of all increments on 16 CPUs.
* **mutex** is correct and **23× slower** than racy. Every increment pays for a lock and an
  unlock, and the lock's cache line bounces between cores.
* **atomic** (`lock xadd` on x86) is correct and about twice as fast as the mutex, but still
  contended: one cache line, four cores.
* **local**: each thread counts privately and adds its total once under the lock. It is correct,
  and **90× faster than the mutex**. The best synchronisation is the one you don't need.

A detail that matters: the racy counter is `volatile`. Without it, GCC at `-O2` keeps the counter
in a register and adds 1,000,000 once at the end. The race still exists, but it looks completely
different: whole threads' work gets lost instead of individual increments. The C standard calls
any data race *undefined behaviour*, so the compiler owes the program nothing.

## Producer / consumer with condition variables

```text
producers ──put()──▶ ┌───┬───┬───┬───┐ ──get()──▶ consumers
                     │   │ x │ x │   │   ring buffer, N slots
                     └───┴───┴───┴───┘
put(): lock; while (full)  wait(not_full);  insert; signal(not_empty); unlock
get(): lock; while (empty) wait(not_empty); remove; signal(not_full);  unlock
```

Four rules, each one a classic bug when broken:

1. **`while`, not `if`, around `pthread_cond_wait`.** After waking up, the condition may already
   be false again: another thread got there first, or the wake-up was spurious.
2. **`pthread_cond_wait` releases the mutex atomically** while it sleeps, and re-acquires it
   before returning. Checking the condition and going to sleep therefore cannot race.
3. **Shutdown needs a broadcast.** When the producers are done, *all* sleeping consumers must wake
   up and notice. `pthread_cond_signal` would wake only one of them.
4. **Verify, don't assume.** The lab records how many times each item was consumed:

```text
$ prodcons_lab -n 100000        (2 producers, 2 consumers, 8-slot buffer)
produced   : 200000
consumed   : 200000
missing    : 0
duplicates : 0
checksum   : OK (19999900000)
waits      : producers slept 50563 times (full), consumers 50840 times (empty)
time       : 4283.8 ms (0.05 M items/s)
```

The wait counters explain the modest throughput. With only 8 slots, a thread goes to sleep about
every 4 items, and each sleep/wake cycle costs tens of microseconds in this VM
([experiments.md §2](experiments.md#2-ipc-latency-waking-another-process-is-the-expensive-part)).
Bigger buffers or batching items per lock acquisition would reduce that.

## Memory ordering in one paragraph

`race_lab` uses `memory_order_relaxed` for its atomic counter: only the final total matters, not
the order in which other memory operations become visible. `bench_ipc`'s spin variant uses
`release` when handing the message over and `acquire` when taking it. That pairing guarantees the
`memcpy` into shared memory is visible *before* the flag that says "your turn". With relaxed
ordering there, the receiver could see the flag first and read a half-written message.
