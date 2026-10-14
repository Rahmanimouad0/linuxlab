/*
 * fd_lab - file descriptors, the mechanism behind every shell redirection.
 *
 *   process                 kernel
 *   fd table                open file descriptions        inodes
 *   ┌───┐                   ┌──────────────────┐
 *   │ 0 │──────────────────▶│ offset, flags    │──────────▶ /dev/pts/0
 *   │ 1 │──┐                └──────────────────┘
 *   │ 3 │──┴──(dup)────────▶│ offset, flags    │──────────▶ out.txt
 *   └───┘                   └──────────────────┘
 *
 *   1. the fd table of this process (/proc/self/fd)
 *   2. open() returns the lowest free descriptor
 *   3. dup() shares the file offset; a second open() does not
 *   4. dup2(fd, 1): how "> file" works
 *   5. fork() shares open file descriptions between processes
 *   6. O_CLOEXEC: which descriptors survive exec()
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

static char dir[] = "/tmp/linuxlab-fd.XXXXXX";

static void path_in_dir(char *out, size_t len, const char *name)
{
    snprintf(out, len, "%s/%s", dir, name);
}

static void list_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        die_errno("opendir /proc/self/fd");
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        int fd = atoi(e->d_name);
        if (fd == dirfd(d))
            continue; /* the descriptor opendir() itself is using */
        char link[128], target[PATH_MAX] = "?";
        snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, target, sizeof target - 1);
        if (n >= 0)
            target[n] = '\0';
        printf("  fd %-3d -> %s\n", fd, target);
    }
    closedir(d);
}

static void show_file(const char *label, const char *path)
{
    char buf[256];
    if (read_file(path, buf, sizeof buf) < 0)
        die_errno(path);
    printf("  %s: \"", label);
    for (char *p = buf; *p; p++) {
        if (*p == '\n')
            fputs("\\n", stdout);
        else
            putchar(*p);
    }
    printf("\"\n");
}

static void exp_table(void)
{
    section("1. this process's descriptor table");
    list_fds();
    printf("0, 1, 2 are inherited from the parent (the shell that started us).\n");
}

static void exp_lowest_free(void)
{
    section("2. open() returns the lowest free descriptor");
    char a[PATH_MAX], b[PATH_MAX];
    path_in_dir(a, sizeof a, "a.txt");
    path_in_dir(b, sizeof b, "b.txt");
    int fa = open(a, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
    int fb = open(b, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
    printf("  open(a) = %d, open(b) = %d\n", fa, fb);
    close(fa);
    int fc = open(b, O_RDONLY | O_CLOEXEC);
    printf("  close(%d); open(b) again = %d   <- the freed slot is reused\n", fa, fc);
    close(fb);
    close(fc);
    printf("the classic trick close(1); open(\"file\") -> fd 1 relies on this rule.\n");
}

static void exp_dup_offset(void)
{
    section("3. dup() shares the offset, a second open() does not");
    char p[PATH_MAX];
    path_in_dir(p, sizeof p, "dup.txt");

    int fd = open(p, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    int copy = dup(fd);
    if (write(fd, "hello ", 6) != 6 || write(copy, "world\n", 6) != 6)
        die_errno("write");
    close(fd);
    close(copy);
    show_file("via fd and its dup()", p);

    int f1 = open(p, O_TRUNC | O_WRONLY | O_CLOEXEC);
    int f2 = open(p, O_WRONLY | O_CLOEXEC);
    if (write(f1, "hello ", 6) != 6 || write(f2, "HELLO", 5) != 5)
        die_errno("write");
    close(f1);
    close(f2);
    show_file("via two open() calls", p);
    printf("two open()s create two open file descriptions, each with its own\n"
           "offset, so the second write overwrote the first.\n");
}

static void exp_dup2(void)
{
    section("4. dup2(fd, STDOUT_FILENO) - the heart of '> file'");
    char p[PATH_MAX];
    path_in_dir(p, sizeof p, "redirected.txt");

    fflush(stdout);
    int saved = dup(STDOUT_FILENO); /* remember where stdout pointed */
    int fd = open(p, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    dup2(fd, STDOUT_FILENO); /* fd 1 now refers to the file */
    close(fd);

    printf("this line was printed with plain printf()\n");
    fflush(stdout);

    dup2(saved, STDOUT_FILENO); /* restore: exactly what lsh does for builtins */
    close(saved);
    show_file("file contents", p);
    printf("printf() never knew: it writes to fd 1, whatever fd 1 refers to.\n");
}

static void exp_fork_shares(void)
{
    section("5. fork() shares open file descriptions");
    char p[PATH_MAX];
    path_in_dir(p, sizeof p, "shared.txt");
    int fd = open(p, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    printf("  parent offset before fork: %ld\n", (long)lseek(fd, 0, SEEK_CUR));
    fflush(stdout);

    pid_t pid = fork();
    if (pid == 0) {
        if (write(fd, "child\n", 6) != 6)
            _exit(1);
        _exit(0);
    }
    waitpid(pid, NULL, 0);
    printf("  parent offset after the child wrote 6 bytes: %ld\n", (long)lseek(fd, 0, SEEK_CUR));
    if (write(fd, "parent\n", 7) != 7)
        die_errno("write");
    close(fd);
    show_file("file contents", p);
    printf("parent and child share one offset - which is why output of all the\n"
           "commands in `(a; b) > file` ends up in order instead of overwriting.\n");
}

static void exp_cloexec(void)
{
    section("6. O_CLOEXEC decides what survives exec()");
    char keep[PATH_MAX], drop[PATH_MAX];
    path_in_dir(keep, sizeof keep, "inherited.txt");
    path_in_dir(drop, sizeof drop, "cloexec.txt");
    int k = open(keep, O_CREAT | O_WRONLY, 0644);
    int d = open(drop, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
    printf("  opened fd %d without O_CLOEXEC, fd %d with O_CLOEXEC\n", k, d);
    printf("  descriptors seen by /bin/sh after exec:\n");
    fflush(stdout);

    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c",
              /* no $(...) here: it would open a pipe in sh itself */
              "for f in /proc/$$/fd/*; do [ -e \"$f\" ] || continue; "
              "printf '    fd %s -> ' \"${f##*/}\"; readlink \"$f\"; done",
              (char *)NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);
    close(k);
    close(d);
    printf("leaked descriptors keep files, pipes and sockets open in unrelated\n"
           "programs; lsh opens everything with O_CLOEXEC except what it dup2()s.\n");
}

static void cleanup(void)
{
    const char *names[] = {"a.txt",      "b.txt",         "dup.txt",    "redirected.txt",
                           "shared.txt", "inherited.txt", "cloexec.txt"};
    char p[PATH_MAX];
    for (size_t i = 0; i < ARRAY_LEN(names); i++) {
        path_in_dir(p, sizeof p, names[i]);
        unlink(p);
    }
    rmdir(dir);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!mkdtemp(dir))
        die_errno("mkdtemp");
    exp_table();
    exp_lowest_free();
    exp_dup_offset();
    exp_dup2();
    exp_fork_shares();
    exp_cloexec();
    cleanup();
    return 0;
}
