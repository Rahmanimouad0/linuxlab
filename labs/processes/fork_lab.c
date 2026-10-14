/*
 * fork_lab - what exactly happens when a process calls fork()?
 *
 * Experiments:
 *   1. fork() returns twice: 0 in the child, the child's pid in the parent
 *   2. who runs first after fork()? (measured, not assumed)
 *   3. parent and child have separate address spaces (copy-on-write)
 *   4. exit statuses, and wait() reaping children in termination order
 *   5. exec() replaces the program but keeps the pid
 *   6. the stdio-buffer-duplication trap (why shells fflush before fork)
 *
 * usage: fork_lab [trials-for-experiment-2]
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

static void describe_status(pid_t pid, int status)
{
    printf("  reaped pid %-7d raw status 0x%04x -> ", (int)pid, status);
    if (WIFEXITED(status))
        printf("exited, code %d\n", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        printf("killed by signal %d (%s)\n", WTERMSIG(status), strsignal(WTERMSIG(status)));
    else
        printf("other\n");
}

static void exp_return_values(void)
{
    section("1. fork() returns twice");
    printf("before fork: a single process, pid=%d\n", (int)getpid());
    fflush(stdout);

    pid_t r = fork();
    if (r < 0)
        die_errno("fork");
    if (r == 0) {
        printf("  child : fork() returned %d  (my pid=%d, my parent=%d)\n", (int)r, (int)getpid(),
               (int)getppid());
        fflush(stdout);
        _exit(0);
    }
    waitpid(r, NULL, 0);
    printf("  parent: fork() returned %d  (= the child's pid; my pid=%d)\n", (int)r, (int)getpid());
}

static void exp_who_runs_first(int trials)
{
    section("2. who runs first after fork()?");

    /* A shared anonymous mapping survives fork() as *shared* memory, so both
     * processes can record the time at which they first ran. */
    uint64_t *t =
        mmap(NULL, 2 * sizeof *t, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED)
        die_errno("mmap");

    int child_first = 0;
    double gap_sum_us = 0;
    for (int i = 0; i < trials; i++) {
        t[0] = t[1] = 0;
        pid_t pid = fork();
        if (pid < 0)
            die_errno("fork");
        if (pid == 0) {
            t[1] = now_ns();
            _exit(0);
        }
        t[0] = now_ns();
        waitpid(pid, NULL, 0);
        if (t[1] < t[0])
            child_first++;
        gap_sum_us += (double)(t[1] > t[0] ? t[1] - t[0] : t[0] - t[1]) / 1000.0;
    }
    printf("trials: %d\n", trials);
    printf("  parent ran first: %d\n", trials - child_first);
    printf("  child  ran first: %d\n", child_first);
    printf("  mean gap between the two first instructions: %.1f us\n", gap_sum_us / trials);
    printf("conclusion: the order is a scheduler decision; correct programs must\n"
           "synchronize explicitly (wait(), pipes, signals...) instead of assuming it.\n");
    munmap(t, 2 * sizeof *t);
}

static void exp_address_spaces(void)
{
    section("3. separate address spaces (copy-on-write)");
    int x = 42;
    printf("before fork: x=%d at address %p\n", x, (void *)&x);
    fflush(stdout);

    pid_t pid = fork();
    if (pid < 0)
        die_errno("fork");
    if (pid == 0) {
        x = 99;
        printf("  child : set x=%d   address %p\n", x, (void *)&x);
        fflush(stdout);
        _exit(0);
    }
    waitpid(pid, NULL, 0);
    printf("  parent: still x=%d address %p\n", x, (void *)&x);
    printf("same *virtual* address, different *physical* pages: the child's write\n"
           "triggered a page fault and the kernel copied the page (copy-on-write).\n");
}

static void exp_exit_status(void)
{
    section("4. exit status and wait() order");
    /* delay (ms) and what each child does */
    struct {
        int delay;
        const char *what;
    } kids[] = {
        {60, "exit(0)"},        {20, "exit(7)"}, {40, "exit(256)"},
        {10, "raise(SIGTERM)"}, {30, "abort()"},
    };
    size_t n = ARRAY_LEN(kids);

    for (size_t i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0)
            die_errno("fork");
        if (pid == 0) {
            sleep_ms(kids[i].delay);
            switch (i) {
            case 0: _exit(0);
            case 1: _exit(7);
            case 2: _exit(256); /* only the low 8 bits survive: parent sees 0 */
            case 3: raise(SIGTERM); break;
            case 4: signal(SIGABRT, SIG_DFL); abort();
            }
            _exit(1);
        }
        printf("  started pid %-7d: sleeps %2d ms then %s\n", (int)pid, kids[i].delay,
               kids[i].what);
    }
    fflush(stdout);
    printf("waitpid(-1, ...) returns children in the order they terminate:\n");
    for (size_t i = 0; i < n; i++) {
        int status;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid < 0)
            die_errno("waitpid");
        describe_status(pid, status);
    }
    printf("note exit(256) -> 0: a process can only report 0..255 to its parent.\n");
}

static void exp_exec(void)
{
    section("5. exec() replaces the program, keeps the process");
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
        die_errno("fork");
    if (pid == 0) {
        printf("  child pid=%d is about to exec /bin/sh\n", (int)getpid());
        fflush(stdout);
        execl("/bin/sh", "sh", "-c", "echo \"  after exec: pid=$$, program=$0 (same pid!)\"",
              (char *)NULL);
        perror("execl");
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    printf("the line after execl() never ran: on success exec does not return.\n");
}

static void exp_stdio_trap(void)
{
    section("6. the buffered-output trap");
    int fds[2];
    if (pipe(fds) < 0)
        die_errno("pipe");

    pid_t helper = fork();
    if (helper < 0)
        die_errno("fork");
    if (helper == 0) {
        /* stdout -> pipe: stdio switches from line to full buffering. */
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        printf("hello ");   /* sits in the stdio buffer, not yet written */
        pid_t pid = fork(); /* ...and the buffer is copied into the child */
        if (pid == 0)
            exit(0); /* exit() flushes the child's copy   */
        waitpid(pid, NULL, 0);
        exit(0); /* and the parent flushes its own copy */
    }
    close(fds[1]);
    char buf[64] = {0};
    ssize_t total = 0, n;
    while ((n = read(fds[0], buf + total, sizeof buf - 1 - (size_t)total)) > 0)
        total += n;
    close(fds[0]);
    waitpid(helper, NULL, 0);
    printf("printf(\"hello \") once, then fork(); the pipe received: \"%s\"\n", buf);
    printf("the unflushed buffer was duplicated. Fixes: fflush() before fork(),\n"
           "or _exit() in the child. lsh calls fflush(NULL) before every fork().\n");
}

int main(int argc, char **argv)
{
    int trials = argc > 1 ? atoi(argv[1]) : 200;
    if (trials <= 0)
        trials = 200;

    setvbuf(stdout, NULL, _IOLBF, 0);
    exp_return_values();
    exp_who_runs_first(trials);
    exp_address_spaces();
    exp_exit_status();
    exp_exec();
    exp_stdio_trap();
    return 0;
}
