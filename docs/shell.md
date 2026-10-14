# How `lsh` works

`lsh` is about 2,000 lines of C in [`shell/src/`](../shell/src). This document follows one
command line from the keyboard to the kernel and back.

```text
linuxlab$ cat < in.txt | grep x > out.txt && echo ok
            │
   main.c   │ getline()                      read one line
            ▼
  lexer.c   lex_line()      WORD REDIR WORD PIPE WORD WORD REDIR WORD AND WORD WORD EOF
            ▼
 parser.c   parse_line()    cmd_list ─┬─ pipeline [cat <in.txt] | [grep x >out.txt]  next=AND
            │                         └─ pipeline [echo ok]                           next=END
            ▼
executor.c  exec_list()     for each pipeline: honour && || ; &
            │  expand_word()   quotes, $VAR, $?, ~   (late: right before running)
            │  pipe() fork() setpgid() dup2() execv()
            ▼
  jobs.c    job_foreground()  tcsetpgrp() + waitpid(WUNTRACED) until done or stopped
```

## 1. Lexing and parsing

The lexer turns the line into tokens: words and the operators `| & ; && || < > >> n> n>&m`.
A word keeps its **raw** text, quotes included. The parser is a small recursive-descent parser for
this grammar:

```text
line     := pipeline ( ('&&' | '||' | ';' | '&') pipeline )* [';' | '&']
pipeline := command ( '|' command )*
command  := ( WORD | REDIR WORD )+
```

Syntax errors carry the offending token, like bash does:
`lsh: syntax error near unexpected token `|'`, and they set `$?` to 2.

### Why expansion happens late

In the first version the lexer expanded `$VAR` and `$?` while it tokenized the line. It looked
fine until this test:

```text
linuxlab$ false; echo $?
0            <- wrong: should be 1
```

The whole line was expanded before `false` ran, so `$?` still held the status from *before* the
line. Real shells therefore split the work into two phases:

1. `lex_line()` only finds word boundaries and checks quoting. It validates the syntax with a
   dummy context and stores the raw word.
2. `expand_word()` runs in the executor, immediately before each pipeline starts.

The same bug would have hit `cd /tmp; echo $PWD`.

### Deliberate simplifications

* Expansions are never split on whitespace. With `X="a b"`, `$X` is one argument, as if it were
  always quoted. An unquoted *empty* expansion still vanishes, as POSIX requires.
* There is no globbing (`*.c`), command substitution (`$(...)`), arithmetic, functions, `if`/`for`,
  or subshells `( ... )`. These are language features; the point of this project is the process
  and file-descriptor machinery.

## 2. Running a pipeline: `pipe`, `fork`, `dup2`, `exec`

For `ls | grep txt | wc -l` the executor creates **n−1 pipes** and **n processes**:

```text
            pipe A                 pipe B
   ls  ───fd1──▶[====]──fd0──▶ grep ───fd1──▶[====]──fd0──▶ wc
```

The loop in `run_pipeline()` does this for each command:

```c
if (not last) pipe2(fds, O_CLOEXEC);        // new pipe for this stage's stdout
fflush(NULL);                               // see "buffer trap" in fork_lab
pid = fork();
child:  dup2(in_fd, 0);  dup2(out_fd, 1);   // the previous read end, this write end
        apply redirections; reset signals; execv()
parent: close(in_fd); close(out_fd);        // parent keeps NO pipe ends
        in_fd = fds[0];                     // next stage reads from here
```

**Why closing matters.** `read()` on a pipe returns EOF only when *every* write end in *every*
process is closed. If the shell kept a copy of pipe A's write end, `grep` would never see EOF and
the pipeline would hang forever. [`pipe_lab`](../labs/ipc/pipe_lab.c) shows this directly. The
test `echo x | cat | cat | cat | cat` guards against it in the suite. All pipes are created with
`O_CLOEXEC`, so a stray descriptor can never leak into an unrelated program. Only the copies made
with `dup2()` onto fd 0 and fd 1 survive `exec`.

`$?` for a pipeline is the status of the **last** command, as in sh without `pipefail`. A process
killed by a signal reports `128 + signal` (for example 143 = 128 + SIGTERM). You can check this
with `sleep 5 & kill %1; wait %1; echo $?`.

### Finding the program

`execvp()` would do the `$PATH` search, but it has a surprising failure mode: if *any* `$PATH`
directory is unreadable, it reports `EACCES` instead of "not found". On WSL, `$PATH` contains
Windows directories, so a typo printed `Permission denied`. `find_in_path()` now searches like a
shell does: the first executable regular file wins and unreadable directories are skipped. A
missing command returns **127** and a found but non-executable file returns **126**.

## 3. Redirection at the file-descriptor level

`> out.txt` is three syscalls in the child, between `fork()` and `exec()`:

```text
fd = open("out.txt", O_WRONLY|O_CREAT|O_TRUNC, 0666)   ->  fd 3
dup2(3, 1)                                             ->  fd 1 now refers to out.txt
close(3)

before:  fd 1 ──▶ /dev/pts/0          after:  fd 1 ──▶ out.txt
```

The program that runs next writes to fd 1 as usual and never knows. Redirections are applied
left to right, which is why the order matters:

| command | stdout | stderr |
|---|---|---|
| `cmd > f 2>&1` | f | f (copy of the *new* fd 1) |
| `cmd 2>&1 > f` | f | the terminal/pipe (copy of the *old* fd 1) |

Both cases are in the integration tests. The file mode `0666` is filtered by the process umask,
so new files usually end up `0644`. `llfs umask` shows the arithmetic.

## 4. Builtins: why `cd` cannot be a program

```text
shell (cwd=/home)                   shell (cwd=/home)
   │ fork                              │  chdir("/tmp")   <- builtin: the shell itself changes
   ▼                                   ▼
child: chdir("/tmp"); exit        shell (cwd=/tmp)
   ✗ the change dies with the child
```

The current directory, environment variables, the job table and "exit" all belong to the shell
process, so `cd`, `export`, `unset`, `jobs`, `fg`, `bg`, `wait` and `exit` run **inside** it. A
builtin with a redirection (`pwd > f`) temporarily `dup`s the affected fds to high numbers, applies
the redirection, runs, flushes stdio and `dup2`s everything back (`run_builtin_here()`).

A builtin *inside a pipeline* runs in a child like any other stage, because it needs its own
stdin and stdout. That is also what bash does. The effect is observable:

```text
linuxlab$ cd / | true; pwd
/home/mouad          <- the cd happened in a child
```

## 5. Job control: process groups, sessions, the terminal

When lsh runs on a terminal, it enables job control (`init_job_control()` in `main.c`):

```text
session (created by login / the terminal emulator)
 ├── process group 1000:  lsh                       <- the shell
 ├── process group 1234:  sleep 100                 <- background job %1
 └── process group 1240:  cat file | grep x         <- FOREGROUND group
                               ▲
      controlling terminal ────┘ tcsetpgrp(tty, 1240)
      Ctrl+C → SIGINT to group 1240, Ctrl+Z → SIGTSTP to group 1240
```

* **Each pipeline gets its own process group** (`setpgid`), and its pgid is the pid of the first
  stage. Both parent *and* child call `setpgid()`. Whichever runs first after `fork()`, the group
  exists before anyone relies on it (see the scheduling experiment in `fork_lab`).
* **The terminal belongs to one foreground group.** Before waiting for a foreground job, the shell
  calls `tcsetpgrp()`. The terminal driver then sends Ctrl+C/Ctrl+Z to that group only, never to
  the shell. Afterwards the shell takes the terminal back and restores its termios settings.
* **The shell ignores** `SIGTSTP`, `SIGTTIN` and `SIGTTOU`. Otherwise Ctrl+Z would stop the shell,
  and `tcsetpgrp()` from a background group would stop it with SIGTTOU. **Children reset
  everything to `SIG_DFL`** before `exec`. Handled signals are reset by `execve()` automatically,
  but *ignored* ones stay ignored.
* **Waiting** uses `waitpid(-1, &st, WUNTRACED)`: a stopped child is reported too. Ctrl+Z then
  returns control to the shell, which prints `[1]+  Stopped  sleep 100`.
* `fg` = `tcsetpgrp(job)` + `kill(-pgid, SIGCONT)` + wait. `bg` = `kill(-pgid, SIGCONT)` only.
  The minus sign sends the signal to the whole process group, so every stage of a stopped
  pipeline continues.
* When the shell exits while jobs are stopped, it warns once (`There are stopped jobs.`). If you
  exit anyway, the kernel's *orphaned process group* rule applies: a group whose members have no
  parent in the session and contain a stopped process receives SIGHUP and SIGCONT. The jobs don't
  stay stopped forever.

Without a terminal (`lsh -c`, scripts, pipes) there is no job control, as in bash. Everything
stays in the shell's process group, `fg`/`bg` report `no job control`, and background jobs read
`/dev/null` instead of stealing the shell's input.

## 6. Signals in the shell itself

| signal | interactive lsh | children |
|---|---|---|
| SIGINT | handler sets a flag, no `SA_RESTART`: Ctrl+C at the prompt interrupts `getline()` and abandons the line | default (terminate) |
| SIGCHLD | handler sets a flag; children are reaped with `waitpid(WNOHANG)` before the next prompt | default |
| SIGTSTP, SIGTTIN, SIGTTOU, SIGQUIT | ignored | default |

The handlers only set `volatile sig_atomic_t` flags. Calling `printf`, `malloc` or touching the job
table inside a handler is not async-signal-safe: the signal could interrupt the same function in
the main program. [`signal_lab`](../labs/signals/signal_lab.c) section 4 shows why a SIGCHLD
handler must loop over `waitpid(WNOHANG)`: 20 children exited and only **one** SIGCHLD was
delivered.

## 7. Testing the shell

| suite | what it checks | how |
|---|---|---|
| `tests/unit/test_parser.c` | lexer, expansion, parser, syntax errors | pure C, no processes |
| `tests/shell/test_shell.sh` | 86 end-to-end behaviours: pipes, redirection order, `$?`, builtins, jobs, errors | runs `lsh -c '...'`, compares output and exit status |
| `tests/shell/test_job_control.py` | Ctrl+C, Ctrl+Z, `fg`, `bg`, stopped pipelines, notifications | drives an interactive lsh on a **pseudo-terminal** |

All of them also pass under AddressSanitizer + UndefinedBehaviorSanitizer (`make SAN=1`), which
includes LeakSanitizer.

## Known limitations

* No globbing, `$(...)`, arithmetic, control flow, functions, aliases, here-documents or history.
* `a && b &` backgrounds only `b`; bash backgrounds the whole and-or list.
* Shell variables and environment variables are the same thing (`export` is the only way to set a
  variable).
* No line editing: the terminal's canonical mode provides only backspace and Ctrl+U.
