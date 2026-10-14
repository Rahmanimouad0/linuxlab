/*
 * signal_lab - signals as asynchronous notifications.
 *
 *   1. default action vs. a handler installed with sigaction()
 *   2. SIGTERM (can be handled: graceful shutdown) vs SIGKILL (cannot)
 *   3. standard signals do not queue, real-time signals do
 *   4. SIGCHLD: one signal can stand for several dead children
 *   5. SIGPIPE: writing to a pipe nobody reads
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

static volatile sig_atomic_t counter;

static void count_handler(int sig)
{
    (void)sig;
    counter++;
}

static void install(int sig, void (*handler)(int), int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    if (sigaction(sig, &sa, NULL) < 0)
        die_errno("sigaction");
}

static void report_child(const char *label, int status)
{
    if (WIFSIGNALED(status))
        printf("  %-28s killed by signal %d (%s)\n", label, WTERMSIG(status),
               strsignal(WTERMSIG(status)));
    else if (WIFEXITED(status))
        printf("  %-28s exited normally, code %d\n", label, WEXITSTATUS(status));
}

static void exp_default_vs_handler(void)
{
    section("1. default action vs. handler");
    pid_t pid = fork();
    if (pid == 0) {
        raise(SIGINT); /* default action for SIGINT: terminate */
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    report_child("child, default SIGINT:", status);

    counter = 0;
    install(SIGINT, count_handler, 0);
    raise(SIGINT);
    printf("  %-28s handler ran, counter=%d, process still alive\n",
           "parent, handled SIGINT:", (int)counter);
    install(SIGINT, SIG_DFL, 0);
}

static volatile sig_atomic_t term_requested;
static void on_term(int sig)
{
    (void)sig;
    term_requested = 1;
}

/* A "server" that shuts down cleanly when asked with SIGTERM. */
static void graceful_child(int ready_fd)
{
    /* Block SIGTERM until we sleep in sigsuspend(); a plain
     * "if (!flag) pause();" could miss a signal arriving in between. */
    sigset_t block, orig;
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigprocmask(SIG_BLOCK, &block, &orig);
    install(SIGTERM, on_term, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    int kill_rc = sigaction(SIGKILL, &sa, NULL);
    int kill_errno = errno;

    char msg[128];
    int len = snprintf(msg, sizeof msg, "sigaction(SIGKILL) -> %d (%s)", kill_rc,
                       kill_rc < 0 ? strerror(kill_errno) : "ok");
    if (write(ready_fd, msg, (size_t)len) < 0)
        _exit(1);
    close(ready_fd);

    while (!term_requested)
        sigsuspend(&orig); /* atomically unblock SIGTERM and sleep */
    /* A real server would flush data, remove its pid file, etc. */
    _exit(0);
}

static void exp_term_vs_kill(void)
{
    section("2. SIGTERM vs SIGKILL");
    const int sigs[] = {SIGTERM, SIGKILL};
    for (size_t i = 0; i < ARRAY_LEN(sigs); i++) {
        int fds[2];
        if (pipe(fds) < 0)
            die_errno("pipe");
        pid_t pid = fork();
        if (pid == 0) {
            close(fds[0]);
            graceful_child(fds[1]);
        }
        close(fds[1]);
        char msg[128] = {0};
        if (read(fds[0], msg, sizeof msg - 1) < 0) /* wait until handlers are set */
            die_errno("read");
        close(fds[0]);
        if (i == 0)
            printf("  child says: %s\n", msg);

        kill(pid, sigs[i]);
        int status;
        waitpid(pid, &status, 0);
        char label[64];
        snprintf(label, sizeof label, "sent %s:", sigs[i] == SIGTERM ? "SIGTERM" : "SIGKILL");
        report_child(label, status);
    }
    printf("SIGTERM lets the program clean up (exit code 0 above); SIGKILL and\n"
           "SIGSTOP can never be caught, blocked or ignored.\n");
}

static void exp_queueing(void)
{
    section("3. pending signals: standard vs real-time");
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigaddset(&block, SIGRTMIN);

    install(SIGUSR1, count_handler, 0);
    sigprocmask(SIG_BLOCK, &block, &old);
    counter = 0;
    for (int i = 0; i < 5; i++)
        kill(getpid(), SIGUSR1);
    sigset_t pending;
    sigpending(&pending);
    printf("  SIGUSR1 sent 5x while blocked; pending: %s\n",
           sigismember(&pending, SIGUSR1) ? "yes" : "no");
    sigprocmask(SIG_SETMASK, &old, NULL);
    printf("  after unblocking, handler ran %d time(s)\n", (int)counter);

    install(SIGRTMIN, count_handler, 0);
    sigprocmask(SIG_BLOCK, &block, &old);
    counter = 0;
    for (int i = 0; i < 5; i++) {
        union sigval v = {.sival_int = i};
        sigqueue(getpid(), SIGRTMIN, v);
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
    printf("  SIGRTMIN queued 5x while blocked; handler ran %d time(s)\n", (int)counter);
    printf("a standard signal is a single pending *bit*; real-time signals are a queue.\n");
}

static volatile sig_atomic_t chld_signals, chld_reaped;
static void on_chld(int sig)
{
    (void)sig;
    int saved = errno; /* waitpid may clobber errno of the interrupted code */
    chld_signals++;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        chld_reaped++;
    errno = saved;
}

static void exp_sigchld(void)
{
    section("4. SIGCHLD");
    const int n = 20;
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    install(SIGCHLD, on_chld, SA_RESTART | SA_NOCLDSTOP);

    /* Block SIGCHLD while forking, so all exits pile up as one pending signal. */
    sigprocmask(SIG_BLOCK, &block, &old);
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid == 0)
            _exit(0);
    }
    sleep_ms(100);
    sigprocmask(SIG_SETMASK, &old, NULL);
    while (chld_reaped < n)
        sleep_ms(1);

    printf("  children: %d, SIGCHLD deliveries: %d, reaped: %d\n", n, (int)chld_signals,
           (int)chld_reaped);
    printf("a handler must loop on waitpid(WNOHANG) - counting signals would miss\n"
           "children. lsh's handler only sets a flag and reaps before each prompt.\n");
    install(SIGCHLD, SIG_DFL, 0);
}

static void exp_sigpipe(void)
{
    section("5. SIGPIPE");
    for (int ignore = 0; ignore <= 1; ignore++) {
        pid_t pid = fork();
        if (pid == 0) {
            int fds[2];
            if (pipe(fds) < 0)
                _exit(2);
            close(fds[0]); /* no reader left */
            if (ignore)
                signal(SIGPIPE, SIG_IGN);
            ssize_t r = write(fds[1], "x", 1);
            printf("  write() returned %zd, errno=%s\n", r, r < 0 ? strerror(errno) : "-");
            fflush(stdout);
            _exit(0);
        }
        int status;
        waitpid(pid, &status, 0);
        report_child(ignore ? "SIGPIPE ignored:" : "SIGPIPE default:", status);
    }
    printf("that is why `yes | head -1` terminates: yes dies of SIGPIPE.\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    exp_default_vs_handler();
    exp_term_vs_kill();
    exp_queueing();
    exp_sigchld();
    exp_sigpipe();
    return 0;
}
