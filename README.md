# LinuxLab — Systems Programming & Linux Internals

[![CI](../../actions/workflows/ci.yml/badge.svg)](../../actions/workflows/ci.yml)

A hands-on Linux systems project in C: a **Unix shell with job control**, surrounded by
**11 experiments** that make processes, signals, file descriptors, IPC, threads, virtual memory
and filesystems observable, plus **benchmarks** that measure what the documentation claims.

```text
linuxlab:~/linuxlab$ cat /etc/passwd | grep -c bash > count.txt && cat count.txt
2
linuxlab:~/linuxlab$ sleep 100
^Z
[1]+  Stopped                 sleep 100
linuxlab:~/linuxlab$ bg
[1]+ sleep 100 &
linuxlab:~/linuxlab$ jobs
[1]+  Running                 sleep 100 &
linuxlab:~/linuxlab$ kill %1
linuxlab:~/linuxlab$ true
[1]+  Terminated              sleep 100
linuxlab:~/linuxlab$ cd /tmp | true; pwd
/home/mouad/linuxlab
linuxlab:~/linuxlab$ nosuchcmd; echo $?
lsh: nosuchcmd: command not found
127
```

*(a real session, recorded on a pseudo-terminal; `cd` inside a pipeline runs in a child process,
so the shell's own directory does not change)*

## What's inside

**MiniShell (`lsh`)**, about 2,000 lines of C, no dependencies:

- ✓ `fork` / `execv` with its own `$PATH` search (127 / 126 exit codes like bash)
- ✓ pipelines of any length: `a | b | c`
- ✓ redirections `<  >  >>  2>  2>>  2>&1  n>&-`, applied left to right
- ✓ lists `;  &&  ||`, background jobs `&`
- ✓ **job control**: process groups, `tcsetpgrp`, Ctrl+C, Ctrl+Z, `jobs`, `fg`, `bg`, `wait`, `kill %n`
- ✓ signal handling that is async-signal-safe (flag-only handlers, `SIGCHLD` reaping)
- ✓ quoting (`'…'`, `"…"`, `\`), `$VAR`, `${VAR}`, `$?`, `$$`, `~`
- ✓ builtins `cd  pwd  exit  export  unset  jobs  fg  bg  wait  kill  help`

**Systems labs**: each program prints what it observes.

| lab | demonstrates |
|---|---|
| `fork_lab` | fork returns twice, who runs first (measured), copy-on-write, exit status encoding, exec keeps the pid, the stdio-buffer duplication trap |
| `proc_states_lab` | R/S/T/Z states read from `/proc`, the zombie lifecycle, orphan adoption, `PR_SET_CHILD_SUBREAPER` |
| `signal_lab` | handlers vs defaults, SIGTERM vs SIGKILL, standard signals coalesce while real-time signals queue, SIGCHLD, SIGPIPE |
| `fd_lab` | fd table, lowest-free-fd rule, `dup` vs a second `open`, `dup2` redirection, shared offsets after fork, `O_CLOEXEC` |
| `pipe_lab` · `fifo_lab` · `shm_lab` · `socket_lab` | EOF rule, capacity, `PIPE_BUF` atomicity; FIFO rendezvous; shared memory + process-shared mutex; Unix sockets, `SO_PEERCRED`, fd passing |
| `race_lab` · `prodcons_lab` | a data race losing 72 % of updates, mutex / atomic / thread-local fixes; bounded buffer with condition variables, verified item by item |
| `mem_lab` | lazy allocation (VmSize vs VmRSS), heap vs mmap, file mappings, copy-on-write counted in page faults |

**Tools built on `/proc` and `stat(2)`**

- `llps`: `ps`, a process tree (`-f`), a `top`-like CPU% view (`-t`), and per-process details (`-p`)
- `llfs`: `tree`, `stat`, a permissions analyzer that names the class the kernel will use, and `umask`

**Engineering**

- ✓ Make **and** CMake/CTest · GCC and Clang · `-Wall -Wextra -Wpedantic` clean
- ✓ **155 tests** in 4 suites: 22 unit, 86 shell integration, 9 interactive job-control (on a pty), 38 lab smoke tests
- ✓ AddressSanitizer + UBSan (+ LeakSanitizer), Valgrind in CI
- ✓ clang-tidy, cppcheck, clang-format enforced in CI
- ✓ reproducible benchmarks → CSV → SVG charts

## Results

All numbers were measured on an AMD Ryzen 7 255 under WSL2; methodology and caveats are in
[docs/experiments.md](docs/experiments.md).

![process creation](results/spawn.svg)

| finding | measured |
|---|---|
| `fork()` cost grows with the parent's memory (page-table copy) | 0.32 ms → **7.9 ms** at 256 MB; `posix_spawn` flat at 0.8 ms |
| Small-message IPC is dominated by waking the other process | pipe 54 µs round trip vs **0.08 µs** busy-waiting on shared memory |
| Shared memory only wins with large chunks | 196 MiB/s at 4 KiB, **12,975 MiB/s** at 1 MiB |
| A contended mutex gets slower with more threads | 189 → 48 M ops/s from 1 → 8 threads |
| False sharing, with identical code | padded counters **9.8×** faster at 8 threads |
| `mmap()` vs `read()` on a cached 512 MiB file | **23.9 GB/s** vs 12.3 GB/s (best `read`) |

## Quick start

```bash
sudo apt install build-essential cmake python3     # Ubuntu / Debian / WSL
make -j"$(nproc)"                                  # builds everything into build/bin
make test                                          # runs all four test suites
./build/bin/lsh                                    # the shell
./build/bin/fork_lab                               # any lab
make bench                                         # benchmarks -> results/
```

With CMake instead: `cmake -B build && cmake --build build -j && ctest --test-dir build`.
Sanitizer build: `make SAN=1 BUILD=build-san && scripts/run_tests.sh build-san/bin`.
A container with every tool (strace, gdb, valgrind, clang-tidy …): see the [`Dockerfile`](Dockerfile).

## Documentation

| document | contents |
|---|---|
| [architecture.md](docs/architecture.md) | components, module structure, design decisions |
| [shell.md](docs/shell.md) | how lsh parses, forks, pipes, redirects and does job control, and the bugs found on the way |
| [processes-and-signals.md](docs/processes-and-signals.md) | fork/exec/wait, zombies, orphans, subreapers, signal semantics |
| [fds-and-ipc.md](docs/fds-and-ipc.md) | the three levels of "open file", choosing an IPC mechanism |
| [concurrency.md](docs/concurrency.md) | data races, mutex vs atomic vs local, condition variables |
| [memory.md](docs/memory.md) | lazy allocation, malloc internals, mmap, copy-on-write |
| [filesystem.md](docs/filesystem.md) | inodes, permission classes, special bits, umask |
| [syscall-tracing.md](docs/syscall-tracing.md) | `strace` of lsh running `cat`: every syscall explained |
| [experiments.md](docs/experiments.md) | benchmark methodology, results, interpretation, threats to validity |

## Repository layout

```text
shell/src/      lsh: lexer, parser, executor, builtins, jobs, signals
labs/           processes/ signals/ fd/ ipc/ concurrency/ memory/
tools/          llps.c, llfs.c
benchmarks/     bench_{spawn,ipc,sync,fileio}.c, run_benchmarks.sh, plot.py
common/         helpers shared by labs, tools and benchmarks
tests/          unit/ (C), shell/ (bash + Python pty), labs/ (smoke tests)
scripts/        run_tests.sh, strace_demo.sh
docs/           the write-ups above
results/        benchmark CSVs, charts, and the machine they came from
```

## Limitations

- lsh is not a POSIX shell: it has no globbing, `$(…)`, arithmetic, `if`/`for`/functions,
  here-documents or line editing, and expansions are never word-split. The project focuses on
  process and file-descriptor mechanics, not on the shell language.
- Benchmarks ran in a WSL2 virtual machine without CPU pinning. The comparisons are meaningful;
  the absolute latencies are higher than on bare metal.
- Linux only (uses `/proc`, `prctl`, `pipe2`, `F_SETPIPE_SZ`, `SO_PEERCRED`, `splice`).

## Future work

- `$(…)` command substitution and globbing via `glob(3)`, to exercise more of the process model
- a `perf`-based analysis of the `MAP_POPULATE` result in `bench_fileio`
- rerun the benchmarks on bare-metal Linux and compare with the WSL2 numbers
- an `io_uring` variant in `bench_fileio`

## License

[MIT](LICENSE)
