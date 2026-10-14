# Processes and signals

Labs: [`fork_lab`](../labs/processes/fork_lab.c), [`proc_states_lab`](../labs/processes/proc_states_lab.c),
[`signal_lab`](../labs/signals/signal_lab.c). The outputs quoted below are real runs on Ubuntu 26.04
under WSL2. Pids and timings will differ on your machine.

## fork(): one call, two returns

```text
Parent (pid 534)
   │ fork()
   ├──────────────────────┐
   ▼                      ▼
Parent: fork() = 535    Child (pid 535, ppid 534): fork() = 0
```

The child is a copy of the parent: the same code, the same open files and the same *virtual*
addresses. It is still a different process with its own copy of memory.

```text
before fork: x=42 at address 0x7ffef95eb774
  child : set x=99   address 0x7ffef95eb774
  parent: still x=42 address 0x7ffef95eb774
```

Same address, different values: the child's write caused a page fault and the kernel copied that
one page (*copy-on-write*). `mem_lab` counts these copies (see [memory.md](memory.md)).

### Who runs first? Measured, not assumed

`fork_lab` forks 50–200 times and records, in shared memory, when parent and child execute their
first instruction:

```text
trials: 50
  parent ran first: 50
  child  ran first: 0
  mean gap between the two first instructions: 47.1 us
```

On this machine the parent always won, because the child must first be scheduled onto another
CPU. The kernel does not guarantee this. Code that depends on the order has a race, which is why
lsh calls `setpgid()` in **both** processes.

### Exit status and `wait()`

```text
waitpid(-1, ...) returns children in the order they terminate:
  reaped pid 590     raw status 0x000f -> killed by signal 15 (Terminated)
  reaped pid 588     raw status 0x0700 -> exited, code 7
  reaped pid 591     raw status 0x0006 -> killed by signal 6 (Aborted)
  reaped pid 589     raw status 0x0000 -> exited, code 0     <- this child called exit(256)
  reaped pid 587     raw status 0x0000 -> exited, code 0
```

The raw status packs two cases into one `int`. The exit code sits in bits 8–15, so only 8 bits
survive and `exit(256)` becomes 0. A signal number sits in bits 0–6. `WIFEXITED`/`WEXITSTATUS`/
`WIFSIGNALED`/`WTERMSIG` decode it. Shells report a death by signal as `128 + n`.

### exec() keeps the process, replaces the program

```text
  child pid=592 is about to exec /bin/sh
  after exec: pid=592, program=sh (same pid!)
```

`fork` + `exec` is the Unix split between *creating a process* and *choosing the program*. The gap
between the two calls is where a shell does its work: `setpgid`, `dup2` for pipes and
redirections, resetting signals.

### The buffered-output trap

```text
printf("hello ") once, then fork(); the pipe received: "hello hello "
```

`printf` only stores the text in a user-space buffer. `fork()` copies the buffer too, and both
processes flush their copy at `exit()`. That is why lsh calls `fflush(NULL)` before every `fork()`
and children that don't `exec` leave with `_exit()`.

## Process states, zombies and orphans

`proc_states_lab` creates children in different states and reads them from `/proc/<pid>/stat`:

```text
  PID     PPID    S  STATE        WHAT IT DOES
  596     595     R  running      this lab (the parent)
  597     596     R  running      busy loop
  598     596     S  sleeping     pause()
  599     596     T  stopped      pause() + SIGSTOP
  600     596     Z  zombie       _exit(0), not yet waited for
```

**Zombie.** A process that has exited but whose parent has not called `wait()` yet. It holds no
memory, only its pid and exit status in the process table:

```text
  t= 50 ms  child 601 state: Z (zombie)
  t=100 ms  child 601 state: Z (zombie)
  t=150 ms  child 601 state: Z (zombie)
  waitpid() -> exit code 42
  after waitpid: /proc/601 no longer exists
```

**Orphan.** A process whose parent died first. The kernel re-parents it to the nearest
*subreaper* ancestor, or to pid 1:

```text
  orphan pid 603: parent was 602, now 425 (Relay(426))       <- WSL's per-session init
  prctl(PR_SET_CHILD_SUBREAPER, 1) done; this process has pid 596
  orphan pid 605: parent was 604, now 596 (proc_states_lab)
```

On a normal Ubuntu desktop the adopter is usually `systemd --user`, which is itself a subreaper.
Container runtimes (tini, `docker --init`) use the same mechanism, because otherwise every
daemon that double-forks would leave zombies behind.

## Signals

A signal is an asynchronous notification sent to a process. The kernel interrupts whatever the
process is doing and either runs the *default action*, runs a *handler*, or ignores it.

| signal | sent by | default | can be caught/ignored? | typical use |
|---|---|---|---|---|
| SIGINT (2) | Ctrl+C (terminal driver) | terminate | yes | interactive interruption |
| SIGTERM (15) | `kill pid` | terminate | yes | "please shut down cleanly" |
| SIGKILL (9) | `kill -9` | terminate | **no** | last resort; no cleanup possible |
| SIGTSTP (20) | Ctrl+Z | stop | yes | job control |
| SIGSTOP (19) | `kill -STOP` | stop | **no** | debuggers, `proc_states_lab` |
| SIGCONT (18) | `fg`, `bg` | continue | yes (the continue still happens) | job control |
| SIGCHLD (17) | kernel, when a child changes state | ignore | yes | reaping children |
| SIGPIPE (13) | kernel, write to a pipe without readers | terminate | yes | ends `yes \| head -1` |

What `signal_lab` measured:

```text
  child says: sigaction(SIGKILL) -> -1 (Invalid argument)
  sent SIGTERM:                exited normally, code 0          <- handler ran, clean exit
  sent SIGKILL:                killed by signal 9 (Killed)      <- no chance to clean up

  SIGUSR1 sent 5x while blocked; pending: yes
  after unblocking, handler ran 1 time(s)                       <- standard signals: 1 pending bit
  SIGRTMIN queued 5x while blocked; handler ran 5 time(s)       <- real-time signals: a queue

  children: 20, SIGCHLD deliveries: 1, reaped: 20               <- never count SIGCHLDs

  SIGPIPE default:             killed by signal 13 (Broken pipe)
  write() returned -1, errno=Broken pipe                        <- with SIGPIPE ignored
```

Two rules follow, and the shell obeys both:

1. **Handlers must be async-signal-safe.** lsh's handlers only set a `volatile sig_atomic_t` flag.
   The real work happens in the main loop.
2. **Avoid the race between checking a flag and sleeping.** `if (!flag) pause();` can miss a
   signal that arrives between the check and `pause()`. The lab blocks the signal first and uses
   `sigsuspend()`, which unblocks it and sleeps in one atomic step.
