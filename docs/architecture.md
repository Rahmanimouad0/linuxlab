# Architecture

```text
                                  LinuxLab
                                     │
        ┌──────────────────┬─────────┴─────────┬──────────────────────┐
        │                  │                   │                      │
   MiniShell (lsh)     Systems labs        Tools (/proc)         Benchmarks
   shell/src/          labs/               tools/                benchmarks/
        │                  │                   │                      │
  lexer → parser →   processes  signals   llps  (ps/top/tree)   spawn  ipc
  expand → executor  fd  ipc  memory      llfs  (tree/stat/     sync   fileio
  jobs  signals      concurrency                 perms/umask)         │
        │                  │                   │                 results/*.csv
        └──────────────────┴─────────┬─────────┴──────────────── results/*.svg
                                     │
                       Linux system-call interface
      fork clone execve wait4 pipe2 dup2 openat mmap sigaction kill setpgid
      tcsetpgrp futex splice socket shm_open prctl ... and /proc, /sys
                                     │
                                Linux kernel
```

## Components

| component | purpose | size |
|---|---|---|
| `shell/src/` | `lsh`, a Unix shell with pipelines, redirections, job control and signals | ~2,050 lines |
| `labs/` | 11 small programs, one per OS concept; each prints what it observes | ~1,980 lines |
| `tools/` | `llps` and `llfs`: `ps`/`top`/`pstree` and `stat`/`tree`/permission analysis built on `/proc` and `stat(2)` | ~740 lines |
| `benchmarks/` | 4 measurement programs + runner + SVG chart generator | ~840 lines |
| `common/` | helpers shared by labs, tools and benchmarks (not by the shell) | ~190 lines |
| `tests/` | unit (C), integration (bash), job control on a pty (Python), lab smoke tests | ~930 lines |

## The shell's modules

```text
main.c ──── read loop, prompt, job-control init
  │
  ├─ parser.c ─── lexer.c          text → tokens → AST (raw words)
  │
  └─ executor.c ─ expand_word()    raw word → argument ($?, $VAR, quotes, ~)
       │          find_in_path()   $PATH search
       │          apply_redirections()
       ├─ builtins.c               cd pwd exit export unset jobs fg bg wait kill help
       ├─ jobs.c                   job table, waitpid bookkeeping, fg/bg, notifications
       └─ signals.c                handler install / reset-for-child
```

Dependencies only point downward, and the parser does not know about processes. That is what
lets `tests/unit/test_parser.c` test the whole front end without forking.

## Design decisions

| decision | alternative | reason |
|---|---|---|
| Expand words just before execution | expand while lexing | `$?` and `$PWD` must see the effects of earlier commands on the same line (a real bug found by the tests; see [shell.md](shell.md)) |
| Own `$PATH` search | `execvp()` | `execvp` reports `EACCES` when *any* PATH directory is unreadable (common on WSL), so a missing command printed "Permission denied" |
| Signal handlers only set flags | do the work in the handler | only async-signal-safe functions may run in a handler; the job table and stdio are not |
| `waitpid(-1, …, WUNTRACED)` loop | wait for specific pids | one loop also collects background jobs that finish while a foreground job runs |
| All fds `O_CLOEXEC` | close manually before exec | no descriptor can leak into child programs, even on error paths |
| Plain Makefile **and** CMake | one of them | `make` for quick local work and sanitizer builds, CMake/CTest for CI and IDEs |
| Labs as separate small programs | one big "lab" binary | each file can be read on its own next to its section in the docs |
| SVG charts from a stdlib-only script | matplotlib | no extra dependency, and the charts render on GitHub |
