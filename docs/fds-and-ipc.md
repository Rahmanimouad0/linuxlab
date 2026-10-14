# File descriptors and inter-process communication

Labs: [`fd_lab`](../labs/fd/fd_lab.c), [`pipe_lab`](../labs/ipc/pipe_lab.c),
[`fifo_lab`](../labs/ipc/fifo_lab.c), [`shm_lab`](../labs/ipc/shm_lab.c),
[`socket_lab`](../labs/ipc/socket_lab.c). Numbers for the same mechanisms are in
[experiments.md](experiments.md).

## Three levels of "open file"

```text
 process A fd table        open file descriptions (kernel)        inodes
 ┌────┐                    ┌─────────────────────────┐
 │ 0  │───────────────────▶│ offset 0, O_RDWR        │─────────▶ /dev/pts/0
 │ 1  │──┐                 └─────────────────────────┘
 │ 3  │──┼──── dup() ─────▶┌─────────────────────────┐
 └────┘  │                 │ offset 12, O_WRONLY     │─────────▶ out.txt
         │   fork() ──────▶└─────────────────────────┘                ▲
 process B fd table              ▲ (shared by A's fds 1+3 and B's fd 1)│
 ┌────┐                          │                                     │
 │ 1  │──────────────────────────┘      a second open("out.txt") ──────┘
 └────┘                                 creates a NEW description with its own offset
```

`fd_lab` makes each arrow visible:

```text
== 2. open() returns the lowest free descriptor ==
  open(a) = 3, open(b) = 4
  close(3); open(b) again = 3   <- the freed slot is reused

== 3. dup() shares the offset, a second open() does not ==
  via fd and its dup(): "hello world\n"
  via two open() calls: "HELLO "           <- second write overwrote the first

== 5. fork() shares open file descriptions ==
  parent offset before fork: 0
  parent offset after the child wrote 6 bytes: 6
  file contents: "child\nparent\n"

== 6. O_CLOEXEC decides what survives exec() ==
  opened fd 3 without O_CLOEXEC, fd 4 with O_CLOEXEC
  descriptors seen by /bin/sh after exec:
    fd 0 -> pipe:[9508]
    fd 1 -> pipe:[8504]
    fd 2 -> pipe:[9510]
    fd 3 -> /tmp/linuxlab-fd.0HIwxA/inherited.txt
    fd 7 -> /dev/ptmx                        <- leaked into us by our own parent!
```

The last line is a real leak: an ancestor process in the WSL environment opened `/dev/ptmx`
without `O_CLOEXEC`, and every program started below it inherits that descriptor. This is
exactly the bug class that `O_CLOEXEC` prevents.

## Choosing an IPC mechanism

| mechanism | direction | who can use it | framing | synchronisation | typical use |
|---|---|---|---|---|---|
| anonymous pipe | one way | related processes (inherited fd) | byte stream | built in (blocking read/write) | shell pipelines |
| FIFO (named pipe) | one way | any process that can open the path | byte stream | built in; `open()` is a rendezvous | simple producer → consumer between independent programs |
| Unix domain socket | two way | any process that can connect to the path | stream or datagrams | built in | local servers (Docker, systemd, X11, databases) |
| POSIX shared memory | both | any process that knows the name | none: raw memory | **you** provide it (semaphores, mutexes, atomics) | large data, lowest latency |
| signals | one way | same user (or root) | a number (+ an int with `sigqueue`) | asynchronous | notifications, not data |

## Findings from the labs

**Pipes: EOF needs every write end closed.**

```text
  child: read "hello through a pipe" then EOF
  (child closed its write end: exit code 0)
  child: no EOF after 500 ms -> would block forever
  (child did NOT close its write end: exit code 1)
```

**Pipes: capacity and back-pressure.** 65,536 bytes by default; `F_SETPIPE_SZ` raised it to
1 MiB. A full pipe blocks the writer, which stops a fast producer from using unbounded memory.

**Pipes: atomic writes.** With 4 concurrent writers, 8,000 records of `PIPE_BUF` = 4,096 bytes
were read and **0** were mixed. POSIX guarantees this for writes up to `PIPE_BUF`. For 64 KiB
records it guarantees nothing. The lab *also* observed 0 mixed records there, because nothing
ever forced a partial write in that run. "Not observed" is not "guaranteed": code must not rely
on it.

**FIFOs: `open()` is a rendezvous.**

```text
  reader: opening after 300 ms
  writer: open(O_WRONLY) blocked for 300 ms
  after the transfer the FIFO still has size 0: data never touched disk
```

**Shared memory: fast, but synchronisation is your job.**

```text
  child : my own mapping is at 0x75dc0cb19000 (inherited one at 0x75dc0cb1a000)
  expected           : 2000000
  unsynchronized ++  : 1995119       <- two processes, one plain counter: lost updates
  with shared mutex  : 2000000       <- pthread mutex with PTHREAD_PROCESS_SHARED
```

The same object was mapped at two different addresses in one process, so pointers stored
*inside* shared memory are meaningless; store offsets instead.

**Unix sockets: credentials and descriptor passing.**

```text
  server: client is pid 468, uid 1000, gid 1000 (from SO_PEERCRED)
  parent: opened /proc/version as fd 4, sending it
  child : received fd 8 and read the first line: "Linux version 6.18.33.2-microsoft-standard-WSL2 ..."
```

`SO_PEERCRED` lets a server authenticate local clients without passwords: the kernel vouches for
the uid. `SCM_RIGHTS` sends an open file *description*, not a path. The receiver gets a new fd
number for the same open file, even if it could not have opened the path itself.
