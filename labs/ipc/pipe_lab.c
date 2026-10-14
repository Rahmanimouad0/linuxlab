/*
 * pipe_lab - anonymous pipes, the IPC mechanism behind `a | b`.
 *
 *   1. parent -> child message; EOF only arrives when *every* write end is closed
 *   2. pipe capacity: how much can be written before write() blocks
 *   3. atomicity: writes of <= PIPE_BUF bytes are never interleaved
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

static void exp_message(void)
{
    section("1. parent -> child, and the EOF rule");
    for (int close_it = 1; close_it >= 0; close_it--) {
        int fds[2];
        if (pipe(fds) < 0)
            die_errno("pipe");
        pid_t pid = fork();
        if (pid == 0) {
            if (close_it)
                close(fds[1]); /* the child closes its copy of the write end */
            char buf[128];
            size_t total = 0;
            for (;;) {
                /* poll with a timeout so the "forgot to close" case ends */
                struct pollfd p = {fds[0], POLLIN, 0};
                if (poll(&p, 1, 500) == 0) {
                    printf("  child: no EOF after 500 ms -> would block forever\n");
                    _exit(1);
                }
                ssize_t n = read(fds[0], buf + total, sizeof buf - 1 - total);
                if (n <= 0)
                    break;
                total += (size_t)n;
            }
            buf[total] = '\0';
            printf("  child: read \"%s\" then EOF\n", buf);
            _exit(0);
        }
        close(fds[0]);
        const char *msg = "hello through a pipe";
        if (write(fds[1], msg, strlen(msg)) < 0)
            die_errno("write");
        close(fds[1]);
        int status;
        waitpid(pid, &status, 0);
        printf("  (child %s its write end: exit code %d)\n", close_it ? "closed" : "did NOT close",
               WEXITSTATUS(status));
    }
    printf("read() returns 0 only when no process holds a write end any more.\n"
           "A shell that forgets to close pipe ends makes `ls | wc` hang.\n");
}

static void exp_capacity(void)
{
    section("2. pipe capacity");
    int fds[2];
    if (pipe2(fds, O_NONBLOCK) < 0)
        die_errno("pipe2");
    printf("  F_GETPIPE_SZ: %d bytes\n", fcntl(fds[1], F_GETPIPE_SZ));

    char chunk[1024];
    memset(chunk, 'x', sizeof chunk);
    long written = 0;
    for (;;) {
        ssize_t n = write(fds[1], chunk, sizeof chunk);
        if (n < 0) {
            if (errno == EAGAIN)
                break;
            die_errno("write");
        }
        written += n;
    }
    printf("  wrote %ld bytes before write() would block (EAGAIN)\n", written);

    int want = 1 << 20;
    int got = fcntl(fds[1], F_SETPIPE_SZ, want);
    if (got < 0)
        printf("  F_SETPIPE_SZ(%d) failed: %s\n", want, strerror(errno));
    else
        printf("  F_SETPIPE_SZ(%d) -> capacity now %d bytes\n", want, got);
    close(fds[0]);
    close(fds[1]);
    printf("a full pipe blocks the writer: this is the back-pressure that keeps\n"
           "`fast | slow` from using unbounded memory.\n");
}

/* n writers each write `records` records of `size` bytes filled with their
 * own letter. The reader checks whether any record got mixed with another. */
static long interleave_test(size_t size, int writers, int records)
{
    int fds[2];
    if (pipe(fds) < 0)
        die_errno("pipe");
    for (int w = 0; w < writers; w++) {
        if (fork() == 0) {
            close(fds[0]);
            char *rec = xmalloc(size);
            memset(rec, 'A' + w, size);
            for (int r = 0; r < records; r++) {
                size_t off = 0;
                while (off < size) {
                    ssize_t n = write(fds[1], rec + off, size - off);
                    if (n <= 0)
                        _exit(1);
                    off += (size_t)n;
                }
            }
            _exit(0);
        }
    }
    close(fds[1]);

    /* Read the stream record by record and check each is one letter. */
    char *rec = xmalloc(size);
    long mixed = 0, total = 0;
    for (;;) {
        size_t off = 0;
        while (off < size) {
            ssize_t n = read(fds[0], rec + off, size - off);
            if (n <= 0)
                break;
            off += (size_t)n;
        }
        if (off < size)
            break;
        total++;
        for (size_t i = 1; i < size; i++) {
            if (rec[i] != rec[0]) {
                mixed++;
                break;
            }
        }
    }
    free(rec);
    close(fds[0]);
    while (wait(NULL) > 0) {
    }
    printf("  record size %7zu bytes: %5ld records read, %5ld mixed\n", size, total, mixed);
    return mixed;
}

static void exp_atomicity(void)
{
    section("3. atomic writes (PIPE_BUF)");
    printf("  PIPE_BUF = %d; 4 concurrent writers\n", PIPE_BUF);
    interleave_test(PIPE_BUF, 4, 2000);
    interleave_test(PIPE_BUF * 16, 4, 200);
    printf("POSIX guarantees writes of <= PIPE_BUF bytes are atomic. Larger\n"
           "writes may be split and interleaved with other writers' data.\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    exp_message();
    exp_capacity();
    exp_atomicity();
    return 0;
}
