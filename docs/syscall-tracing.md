# What happens when lsh runs `cat test.txt`?

`strace` records every system call a process makes: the complete conversation between a program
and the kernel. These traces were captured with strace 6.19 on Ubuntu 26.04 (WSL2). Rerun them
with [`scripts/strace_demo.sh`](../scripts/strace_demo.sh). The listings are shortened: pointer
values are replaced by `…`, and the dynamic loader's `mmap`s are left out.

## 1. The shell: fork, reset, search, exec, wait

```text
lsh (pid 411)                                             child (pid 412)
─────────────                                             ───────────────
clone(flags=CLONE_CHILD_SETTID|…|SIGCHLD) = 412   ─────▶  (starts here, a copy of lsh)
wait4(-1, …, WSTOPPED, …)            ← blocks             rt_sigprocmask(SIG_SETMASK, [])
                                                          rt_sigaction(SIGINT,  SIG_DFL)
                                                          rt_sigaction(SIGQUIT, SIG_DFL)
                                                          rt_sigaction(SIGTSTP, SIG_DFL)  … SIGTTIN, SIGTTOU,
                                                          rt_sigaction(SIGCHLD, SIG_DFL)    SIGCHLD, SIGPIPE
                                                          newfstatat("/usr/local/sbin/cat") = -1 ENOENT
                                                          newfstatat("/usr/local/bin/cat")  = -1 ENOENT
                                                          newfstatat("/usr/sbin/cat")       = -1 ENOENT
                                                          newfstatat("/usr/bin/cat", {S_IFREG|0755…}) = 0
                                                          access("/usr/bin/cat", X_OK)      = 0
                                                          execve("/usr/bin/cat", ["cat", "test.txt"], …)
                                                          … cat runs (section 2) …
                                                          exit_group(0)
= 412, [{WIFEXITED(s) && WEXITSTATUS(s) == 0}]  ◀──────
--- SIGCHLD {si_pid=412, si_code=CLD_EXITED, si_status=0} ---
exit_group(0)
```

Each line maps to a piece of [`executor.c`](../shell/src/executor.c):

| syscall | where | why |
|---|---|---|
| `clone(… SIGCHLD)` | `fork()` | glibc implements `fork()` with `clone()`. The `SIGCHLD` flag means "notify the parent with SIGCHLD when I die". |
| `rt_sigaction(…, SIG_DFL)` ×7 | `signals_reset_for_child()` | An interactive lsh *ignores* SIGTSTP/SIGTTOU/…, and ignored dispositions survive `execve`, so they must be reset explicitly. |
| `newfstatat` ×4, `access` | `find_in_path()` | The shell's own `$PATH` search: `stat` each candidate, check that it is a regular file, then `access(X_OK)`. |
| `execve` | `child_exec()` | Replace the child's program. The pid stays 412. |
| `wait4(-1, …, WSTOPPED)` | `job_wait()` | `waitpid(-1, &st, WUNTRACED)`. glibc spells WUNTRACED as WSTOPPED. It returns when a child exits **or stops** (Ctrl+Z). |

In `-c` mode there is no terminal, so the trace has no `setpgid` and no `ioctl(TIOCSPGRP)`
(`tcsetpgrp`). Trace an interactive session with `strace -f -e trace=setpgid,ioctl build/bin/lsh`
to see the job-control calls. Each foreground job produces `setpgid(pid, pgid)` in *both*
processes and `ioctl(0, TIOCSPGRP, [pgid])` before the wait and after it.

## 2. The program: dynamic loading, then the actual work

128 of cat's syscalls happen between `execve` and the moment it opens `test.txt`. Most of them
are the dynamic loader mapping `libc.so.6`, `libm.so.6`, `libgcc_s.so.1`, `libselinux.so.1` and
`libpcre2-8.so.0`: `openat`, then `read` the ELF header, then `mmap` each segment, then
`mprotect` the relocated data to read-only. The rest is runtime setup such as signal stacks and
locale files. A statically linked program would skip most of that.

The work itself is not what one would expect. On Ubuntu 26.04, `/usr/bin/cat` is
**uutils coreutils 0.8.0**, the Rust rewrite of GNU coreutils, and it does not use
`read`/`write` at all:

```text
openat(AT_FDCWD, "test.txt", O_RDONLY|O_CLOEXEC) = 3
fstat(3, {st_mode=S_IFREG|0644, st_size=12, ...}) = 0
fstat(1, {st_mode=S_IFCHR|0666, st_rdev=makedev(0x1, 0x3), ...}) = 0     <- stdout is /dev/null
splice(3, NULL, 1, NULL, 1048576, 0)    = -1 EINVAL   <- file -> /dev/null directly: refused
pipe([4, 5])                            = 0
fcntl(4, F_SETPIPE_SZ, 1048576)         = 1048576      <- a 1 MiB pipe as a kernel-side buffer
splice(3, NULL, 5, NULL, 1048576, 0)    = 12           <- file   -> pipe  (no copy to user space)
splice(4, NULL, 1, NULL, 12, 0)         = 12           <- pipe   -> stdout
splice(3, NULL, 5, NULL, 1048576, 0)    = 0            <- EOF
close(5); close(4); close(3)
exit_group(0)
```

`splice()` moves data between a file descriptor and a pipe *inside the kernel*, so the 12 bytes
never enter cat's memory. One end of every splice must be a pipe. When the direct
file → `/dev/null` splice fails with `EINVAL`, cat creates its own pipe and splices
file → pipe → stdout. Traditional GNU cat would show `read(3, "hello\nworld\n", 131072) = 12`
followed by `write(1, "hello\nworld\n", 12) = 12`. The zero-copy idea is the same one the
`bench_fileio` results show for `mmap()` vs `read()`.

## 3. A pipeline with a redirection

`lsh -c 'cat test.txt | grep o > out.txt'`, filtered to the calls that build the pipeline
(pids: lsh 425, cat 426, grep 427):

```text
425  pipe2([3, 4], O_CLOEXEC)                     one pipe for "cat | grep"
425  clone(…) = 426                               fork cat
426  close(3)                                     cat does not need the read end
426  dup2(4, 1)                                   cat's stdout := pipe write end
425  clone(…) = 427                               fork grep (parent closed 4 in between)
427  dup2(3, 0)                                   grep's stdin := pipe read end
427  openat(AT_FDCWD, "out.txt", O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC, 0666) = 3
427  dup2(3, 1)                                   "> out.txt": grep's stdout := the file
426  execve("/usr/bin/cat",  ["cat", "test.txt"], …)
427  execve("/usr/bin/grep", ["grep", "o"], …)
425  wait4(-1, …) = 426                           cat exits first
425  --- SIGCHLD {si_pid=426, si_code=CLD_EXITED} ---
425  wait4(-1, …) = 427                           then grep (it saw EOF because every
                                                  write end of the pipe was closed)
```

Things to notice:

* **`O_CLOEXEC` everywhere.** The pipe and `out.txt` are opened close-on-exec. Only the copies
  made by `dup2()` onto fd 0 and fd 1 survive `execve`, so cat and grep start with exactly
  fds 0, 1 and 2.
* **Both children run concurrently.** Their syscalls interleave in the full trace, and `strace`
  shows `<unfinished …>` / `<… resumed>` pairs.
* **The redirection happens after the pipe `dup2`.** `grep o > out.txt` overrides the pipe for
  stdout, exactly as the left-to-right rule in [shell.md](shell.md) says.

## 4. Where does the time go?

`strace -c -f -S calls lsh -c 'cat test.txt'` counts the calls of both processes: **194 syscalls
in total** to print a 12-byte file. The counts are the reliable part. The times vary from run to
run, and strace itself slows every syscall down a lot.

```text
 calls  errors  syscall         what for
    35          mmap            mostly the dynamic loader mapping library segments
    19          read            ELF headers, /proc/mounts, /proc/self/maps, a locale file
    16          rt_sigaction    lsh resetting signals + cat's own SIGPIPE/SIGSEGV setup
    15          close
    14       1  openat          ld.so.cache + libraries (both programs), /proc files,
                                a locale file, test.txt
    11          mprotect        loader: relocated data -> read-only
     4       3  newfstatat      lsh's $PATH search: 3 misses, then /usr/bin/cat
     4       1  splice          the actual copying
     2          execve          lsh itself, then cat
     1          clone           fork()
     1          wait4           the shell waiting (70 % of the traced wall time)
```

Starting a program costs far more than the work in this example: only 4 of the 194 calls copy
data. This is also why `fork+exec` in [experiments.md](experiments.md) takes about 1 ms. Tools
that start thousands of processes (build systems, shell loops) pay this price every time.
