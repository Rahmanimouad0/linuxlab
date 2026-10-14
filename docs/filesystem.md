# Files, inodes and permissions

Tool: [`llfs`](../tools/llfs.c): `tree`, `stat`, `perms`, `umask`.

## A file is an inode; a name is a directory entry

```text
directory "project/"                          inode 25
┌───────────────┬───────┐                     ┌──────────────────────────────┐
│ name          │ inode │                     │ type, mode 0640, uid, gid    │
├───────────────┼───────┤                     │ size 3, blocks 8, links 1    │
│ notes.txt     │  25   │ ──────────────────▶ │ atime / mtime / ctime        │
│ src           │  31   │                     │ pointers to data blocks      │
└───────────────┴───────┘                     └──────────────────────────────┘
```

The name is **not** stored in the inode. That is why a hard link is just a second directory
entry pointing to the same inode (the `Links` count goes up), and why `rename()` inside one
filesystem is instant: only the directory changes. `llfs stat` prints everything `lstat()`
returns:

```text
    File: /tmp/p.txt
    Type: regular file
    Size: 3 bytes   Blocks: 8 (512-byte units)   IO block: 4096
   Inode: 25   Links: 1   Device: 0,71
    Mode: 0640 (-rw-r-----)
   Owner: mouad (1000)   Group: mouad (1000)
  Access: 2026-09-23 16:46:14.739006401
  Modify: 2026-09-23 16:46:14.743451686
  Change: 2026-09-23 16:46:14.747291821
```

A 3-byte file occupies 8 × 512 bytes = one 4 KiB block. **Modify** (mtime) changes when the
*content* changes; **Change** (ctime) changes when the *inode* changes (content, but also
`chmod`, `chown`, a new link). ctime cannot be set by `touch` or `utimes()`, which makes it
useful to forensics.

`llfs tree` uses `lstat()` rather than `stat()`, so it shows symlinks as links and never
follows them. A link to `/` therefore cannot send the walk into a loop.

## Permission checks: exactly one class applies

```text
file:    /tmp/p.txt (regular file)
mode:    -rw-r-----  (0640)
owner:   rw-  mouad        read, write
group:   r--  mouad        read
other:   ---  (everyone)   nothing
you:     uid 1000: you own it -> only the OWNER bits apply (rw-)
access(): read yes, write yes, execute no
```

The kernel does **not** combine the three classes. It picks one, in this order:

1. effective uid is 0 → root, most checks are bypassed;
2. effective uid == file owner → **owner bits only**;
3. file group is the effective gid or one of the supplementary groups → **group bits only**;
4. otherwise → **other bits**.

A surprising consequence: a file with mode `0077` owned by you is unreadable **for you**, even
though everyone else can read it. `llfs perms` names the class the kernel will use, and
cross-checks it with `access()`.

For **directories** the same bits mean something else, and `llfs perms` translates them:

| bit | on a file | on a directory |
|---|---|---|
| r | read content | list the names (`ls`) |
| w | modify content | create, delete, rename entries, *even of files you don't own* |
| x | execute | enter / traverse (`cd`, or use a path through it) |

```text
file:    /tmp (directory)
mode:    drwxrwxrwt  (1777)
other:   rwt  (everyone)   list names, create/delete entries, enter/traverse
special: sticky (only owners may delete entries)
```

The **sticky bit** on `/tmp` fixes the "w on a directory lets you delete others' files" problem.
Everyone may create files there, but only a file's owner may delete or rename it. The other two
special bits are **setuid**, which runs a program with its owner's uid (`passwd`, `sudo`), and
**setgid**. On a directory, setgid makes new files inherit the directory's group.

## umask

```text
umask:              0022
new file  (0666 & ~umask): 0644 -rw-r--r--
new dir   (0777 & ~umask): 0755 drwxr-xr-x
```

Programs request `0666` for files (lsh does too, for `>`), and the kernel clears the bits set in
the process umask. The umask can only remove permissions. It is inherited across `fork` and
`exec` and is usually set once by the login shell. The lsh test *new file mode = 0666&~umask*
checks this against the umask of the test environment.
