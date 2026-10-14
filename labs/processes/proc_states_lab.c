/*
 * proc_states_lab - observe process states, zombies and orphans directly
 * through /proc.
 *
 *   1. a table of children in different states (R, S, T, Z)
 *   2. the zombie lifecycle: exit -> Z -> wait() -> gone
 *   3. orphans: who adopts a process whose parent dies?
 *   4. PR_SET_CHILD_SUBREAPER: becoming the adopter ourselves
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

static pid_t proc_ppid(pid_t pid)
{
    char path[64], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    if (read_file(path, buf, sizeof buf) <= 0)
        return -1;
    const char *p = strrchr(buf, ')');
    int ppid = -1;
    if (p)
        sscanf(p + 2, "%*c %d", &ppid);
    return ppid;
}

static void proc_comm(pid_t pid, char *out, size_t len)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
    if (read_file(path, out, len) <= 0)
        snprintf(out, len, "?");
    out[strcspn(out, "\n")] = '\0';
}

static void print_row(pid_t pid, const char *role)
{
    char s = proc_state(pid);
    printf("  %-7d %-7d %c  %-12s %s\n", (int)pid, (int)proc_ppid(pid), s ? s : '-',
           proc_state_name(s), role);
}

static void exp_state_table(void)
{
    section("1. process states");
    pid_t running = fork();
    if (running == 0)
        for (volatile unsigned long i = 0;; i++) /* burn CPU */
            ;
    pid_t sleeping = fork();
    if (sleeping == 0) {
        pause(); /* blocks until a signal arrives */
        _exit(0);
    }
    pid_t stopped = fork();
    if (stopped == 0) {
        pause();
        _exit(0);
    }
    pid_t zombie = fork();
    if (zombie == 0)
        _exit(0); /* exits immediately; we deliberately do not wait() yet */
    if (running < 0 || sleeping < 0 || stopped < 0 || zombie < 0)
        die_errno("fork");

    kill(stopped, SIGSTOP);
    sleep_ms(200);

    printf("  %-7s %-7s %-2s %-12s %s\n", "PID", "PPID", "S", "STATE", "WHAT IT DOES");
    print_row(getpid(), "this lab (the parent)");
    print_row(running, "busy loop");
    print_row(sleeping, "pause()");
    print_row(stopped, "pause() + SIGSTOP");
    print_row(zombie, "_exit(0), not yet waited for");

    /* Clean up. SIGKILL works even on a stopped process. */
    pid_t all[] = {running, sleeping, stopped, zombie};
    for (size_t i = 0; i < ARRAY_LEN(all); i++) {
        kill(all[i], SIGKILL);
        waitpid(all[i], NULL, 0);
    }
    printf("(a running process may show S if it was preempted at the instant we looked)\n");
}

static void exp_zombie(void)
{
    section("2. zombie lifecycle");
    pid_t pid = fork();
    if (pid < 0)
        die_errno("fork");
    if (pid == 0)
        _exit(42);

    for (int i = 0; i < 3; i++) {
        sleep_ms(50);
        char s = proc_state(pid);
        printf("  t=%3d ms  child %d state: %c (%s)\n", (i + 1) * 50, (int)pid, s ? s : '-',
               proc_state_name(s));
    }
    printf("the child has exited, but its pid and exit status are kept in the\n"
           "process table until the parent collects them.\n");

    int status;
    waitpid(pid, &status, 0);
    printf("  waitpid() -> exit code %d\n", WEXITSTATUS(status));

    char path[64];
    snprintf(path, sizeof path, "/proc/%d", (int)pid);
    printf("  after waitpid: %s %s\n", path,
           access(path, F_OK) == 0 ? "still exists" : "no longer exists");
}

/* Fork a child that forks a grandchild and then dies; report the
 * grandchild's parent before and after. Returns the grandchild's pid. */
static pid_t orphan_experiment(void)
{
    int fds[2];
    if (pipe(fds) < 0)
        die_errno("pipe");

    pid_t middle = fork();
    if (middle < 0)
        die_errno("fork");
    if (middle == 0) {
        pid_t grandchild = fork();
        if (grandchild == 0) {
            pid_t before = getppid();
            /* Wait until our parent has died and we were re-parented. */
            for (int i = 0; i < 400 && getppid() == before; i++)
                sleep_ms(5);
            pid_t report[3] = {getpid(), before, getppid()};
            if (write(fds[1], report, sizeof report) != (ssize_t)sizeof report)
                _exit(1);
            _exit(0);
        }
        sleep_ms(50);
        _exit(0); /* the middle process dies: grandchild becomes an orphan */
    }
    close(fds[1]);
    waitpid(middle, NULL, 0);

    pid_t report[3];
    if (read(fds[0], report, sizeof report) != (ssize_t)sizeof report)
        die("orphan did not report");
    close(fds[0]);

    char name[64];
    proc_comm(report[2], name, sizeof name);
    printf("  orphan pid %d: parent was %d, now %d (%s)\n", (int)report[0], (int)report[1],
           (int)report[2], name);
    return report[0];
}

static void exp_orphan(void)
{
    section("3. orphans get adopted");
    orphan_experiment();
    printf("the kernel re-parents orphans to the nearest 'child subreaper'\n"
           "ancestor, or to pid 1. That new parent is then responsible for\n"
           "reaping them, so orphans do not become permanent zombies.\n");
}

static void exp_subreaper(void)
{
    section("4. becoming a subreaper");
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0)
        die_errno("prctl");
    printf("prctl(PR_SET_CHILD_SUBREAPER, 1) done; this process has pid %d\n", (int)getpid());
    pid_t orphan = orphan_experiment();
    int status;
    if (waitpid(orphan, &status, 0) == orphan)
        printf("  we could waitpid() the adopted grandchild: exit code %d\n", WEXITSTATUS(status));
    printf("this is how container init processes and service managers\n"
           "(systemd --user, tini, ...) keep track of daemons that double-fork.\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    exp_state_table();
    exp_zombie();
    exp_orphan();
    exp_subreaper();
    return 0;
}
