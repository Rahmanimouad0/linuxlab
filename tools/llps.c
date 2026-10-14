/*
 * llps - a small ps / pstree / top built only on /proc.
 *
 *   llps                 list all processes (like `ps -e`)
 *   llps -f              process tree (like `pstree -p`)
 *   llps -t [-i ms] [-n N] [-c count]
 *                        top-like view: CPU% measured over an interval
 *   llps -p PID          everything interesting about one process
 *
 * Everything shown comes from these files:
 *   /proc/<pid>/stat     state, ppid, CPU ticks, vsize, rss (one line)
 *   /proc/<pid>/status   uid, threads, context switches (key: value)
 *   /proc/<pid>/cmdline  argv, NUL-separated
 *   /proc/meminfo, /proc/loadavg, /proc/uptime
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

typedef struct {
    pid_t pid, ppid;
    char state;
    char comm[64];
    char cmd[256];
    uid_t uid;
    unsigned long long utime, stime, starttime; /* clock ticks */
    unsigned long vsize;                        /* bytes */
    long rss_pages;
    long threads;
    double cpu; /* percent, only for -t */
} procinfo;

typedef struct {
    procinfo *v;
    size_t n, cap;
} proclist;

static long page_kb;
static long ticks_per_sec;

static int read_proc(pid_t pid, procinfo *p)
{
    char path[64], buf[2048];
    memset(p, 0, sizeof *p);
    p->pid = pid;

    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    if (read_file(path, buf, sizeof buf) <= 0)
        return -1; /* the process exited while we were looking */

    /* "1234 (some name) S 1 ..." - the name may contain spaces or ')' */
    char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
    if (!lp || !rp || rp < lp)
        return -1;
    size_t n = (size_t)(rp - lp - 1);
    if (n >= sizeof p->comm)
        n = sizeof p->comm - 1;
    memcpy(p->comm, lp + 1, n);

    int ppid;
    /* fields 3..24 of proc(5): state ppid pgrp session tty tpgid flags
     * minflt cminflt majflt cmajflt utime stime cutime cstime priority
     * nice num_threads itrealvalue starttime vsize rss */
    int got = sscanf(rp + 2,
                     "%c %d %*s %*s %*s %*s %*s %*s %*s %*s %*s %llu %llu %*s %*s %*s %*s "
                     "%ld %*s %llu %lu %ld",
                     &p->state, &ppid, &p->utime, &p->stime, &p->threads, &p->starttime, &p->vsize,
                     &p->rss_pages);
    if (got != 8)
        return -1;
    p->ppid = ppid;

    long uid = proc_status_field(pid, "Uid");
    p->uid = uid < 0 ? 0 : (uid_t)uid;

    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pid);
    ssize_t len = read_file(path, p->cmd, sizeof p->cmd);
    if (len <= 0) {
        /* kernel threads have an empty command line */
        snprintf(p->cmd, sizeof p->cmd, "[%s]", p->comm);
    } else {
        for (ssize_t i = 0; i < len - 1; i++)
            if (p->cmd[i] == '\0')
                p->cmd[i] = ' ';
    }
    return 0;
}

static void list_processes(proclist *out)
{
    DIR *d = opendir("/proc");
    if (!d)
        die_errno("opendir /proc");
    struct dirent *e;
    while ((e = readdir(d))) {
        char *end;
        long pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 0)
            continue; /* not a process directory */
        if (out->n == out->cap) {
            size_t cap = out->cap ? out->cap * 2 : 256;
            procinfo *v = realloc(out->v, cap * sizeof *v);
            if (!v)
                die("out of memory");
            out->v = v;
            out->cap = cap;
        }
        if (read_proc((pid_t)pid, &out->v[out->n]) == 0)
            out->n++;
    }
    closedir(d);
    if (out->n == 0)
        die("no processes found in /proc (is procfs mounted?)");
}

static const char *user_name(uid_t uid)
{
    static uid_t cached_uid = (uid_t)-1;
    static char cached[32];
    if (uid != cached_uid) {
        struct passwd *pw = getpwuid(uid);
        if (pw)
            snprintf(cached, sizeof cached, "%s", pw->pw_name);
        else
            snprintf(cached, sizeof cached, "%u", (unsigned)uid);
        cached_uid = uid;
    }
    return cached;
}

static int by_pid(const void *a, const void *b)
{
    const procinfo *x = a, *y = b;
    return (x->pid > y->pid) - (x->pid < y->pid);
}

static int by_cpu(const void *a, const void *b)
{
    const procinfo *x = a, *y = b;
    if (x->cpu != y->cpu)
        return x->cpu < y->cpu ? 1 : -1;
    return by_pid(a, b);
}

/* ------------------------------------------------------------ ps mode */

static void cmd_list(void)
{
    proclist pl = {0};
    list_processes(&pl);
    qsort(pl.v, pl.n, sizeof *pl.v, by_pid);
    printf("%7s %7s %-10s %s %9s %9s %4s %s\n", "PID", "PPID", "USER", "S", "RSS(kB)", "VSZ(kB)",
           "THR", "COMMAND");
    for (size_t i = 0; i < pl.n; i++) {
        procinfo *p = &pl.v[i];
        printf("%7d %7d %-10.10s %c %9ld %9lu %4ld %.80s\n", (int)p->pid, (int)p->ppid,
               user_name(p->uid), p->state, p->rss_pages * page_kb, p->vsize / 1024, p->threads,
               p->cmd);
    }
    printf("%zu processes\n", pl.n);
    free(pl.v);
}

/* ---------------------------------------------------------- tree mode */

static void print_tree(const proclist *pl, pid_t parent, const char *prefix)
{
    /* collect children of `parent` (the list is sorted by pid) */
    size_t count = 0;
    for (size_t i = 0; i < pl->n; i++)
        if (pl->v[i].ppid == parent && pl->v[i].pid != parent)
            count++;

    size_t seen = 0;
    for (size_t i = 0; i < pl->n; i++) {
        const procinfo *p = &pl->v[i];
        if (p->ppid != parent || p->pid == parent)
            continue;
        int last = ++seen == count;
        printf("%s%s%s(%d) %c\n", prefix, last ? "└── " : "├── ", p->comm, (int)p->pid, p->state);
        char next[1024];
        snprintf(next, sizeof next, "%s%s", prefix, last ? "    " : "│   ");
        print_tree(pl, p->pid, next);
    }
}

static void cmd_tree(void)
{
    proclist pl = {0};
    list_processes(&pl);
    qsort(pl.v, pl.n, sizeof *pl.v, by_pid);

    /* roots: processes whose parent is not visible (pid 1, kthreadd, or
     * the init of a container / WSL instance) */
    for (size_t i = 0; i < pl.n; i++) {
        int parent_visible = 0;
        for (size_t j = 0; j < pl.n; j++)
            if (pl.v[j].pid == pl.v[i].ppid && pl.v[i].ppid != pl.v[i].pid)
                parent_visible = 1;
        if (!parent_visible) {
            printf("%s(%d) %c\n", pl.v[i].comm, (int)pl.v[i].pid, pl.v[i].state);
            print_tree(&pl, pl.v[i].pid, "");
        }
    }
    free(pl.v);
}

/* ----------------------------------------------------------- top mode */

static long meminfo_kb(const char *key)
{
    char buf[4096];
    if (read_file("/proc/meminfo", buf, sizeof buf) <= 0)
        return -1;
    const char *p = strstr(buf, key);
    return p ? strtol(p + strlen(key) + 1, NULL, 10) : -1;
}

static void cmd_top(int interval_ms, int rows, int count)
{
    for (int iter = 0; iter < count; iter++) {
        proclist a = {0}, b = {0};
        list_processes(&a);
        qsort(a.v, a.n, sizeof *a.v, by_pid);
        uint64_t t0 = now_ns();
        sleep_ms(interval_ms);
        list_processes(&b);
        double secs = (double)(now_ns() - t0) / 1e9;

        /* CPU% = ticks used during the interval / ticks available */
        for (size_t i = 0; i < b.n; i++) {
            procinfo key = {.pid = b.v[i].pid};
            procinfo *old = bsearch(&key, a.v, a.n, sizeof *a.v, by_pid);
            unsigned long long before = old ? old->utime + old->stime : 0;
            unsigned long long now = b.v[i].utime + b.v[i].stime;
            if (old && old->starttime != b.v[i].starttime)
                before = 0; /* pid was reused by a new process */
            b.v[i].cpu = 100.0 * (double)(now - before) / ((double)ticks_per_sec * secs);
        }
        qsort(b.v, b.n, sizeof *b.v, by_cpu);

        if (count > 1 && isatty(STDOUT_FILENO))
            printf("\033[H\033[2J"); /* clear the screen */

        char load[128] = "?", up[64] = "0";
        read_file("/proc/loadavg", load, sizeof load);
        load[strcspn(load, "\n")] = '\0';
        read_file("/proc/uptime", up, sizeof up);
        long total = meminfo_kb("MemTotal:"), avail = meminfo_kb("MemAvailable:");
        long running = 0;
        for (size_t i = 0; i < b.n; i++)
            running += b.v[i].state == 'R';

        printf("uptime %.0f s   load %s\n", atof(up), load);
        printf("tasks %zu (%ld running)   mem %ld MB total, %ld MB available   CPUs %ld\n\n", b.n,
               running, total / 1024, avail / 1024, sysconf(_SC_NPROCESSORS_ONLN));
        printf("%7s %-10s %6s %6s %s %9s %s\n", "PID", "USER", "CPU%", "MEM%", "S", "RSS(kB)",
               "COMMAND");
        for (size_t i = 0; i < b.n && (int)i < rows; i++) {
            procinfo *p = &b.v[i];
            long rss = p->rss_pages * page_kb;
            printf("%7d %-10.10s %6.1f %6.1f %c %9ld %.60s\n", (int)p->pid, user_name(p->uid),
                   p->cpu, total > 0 ? 100.0 * (double)rss / (double)total : 0.0, p->state, rss,
                   p->comm);
        }
        fflush(stdout);
        free(a.v);
        free(b.v);
    }
}

/* -------------------------------------------------------- detail mode */

static void cmd_detail(pid_t pid)
{
    procinfo p;
    if (read_proc(pid, &p) < 0)
        die("no such process: %d", (int)pid);

    char path[64], link[PATH_MAX];
    printf("pid        %d\n", (int)p.pid);
    printf("command    %s\n", p.cmd);
    printf("state      %c (%s)\n", p.state, proc_state_name(p.state));
    printf("parent     %d\n", (int)p.ppid);
    printf("user       %s\n", user_name(p.uid));
    printf("threads    %ld\n", p.threads);
    printf("cpu time   user %.2f s, system %.2f s\n", (double)p.utime / (double)ticks_per_sec,
           (double)p.stime / (double)ticks_per_sec);

    const char *fields[] = {"VmPeak",
                            "VmSize",
                            "VmRSS",
                            "RssAnon",
                            "RssFile",
                            "VmSwap",
                            "voluntary_ctxt_switches",
                            "nonvoluntary_ctxt_switches"};
    for (size_t i = 0; i < ARRAY_LEN(fields); i++) {
        long v = proc_status_field(pid, fields[i]);
        if (v >= 0)
            printf("%-26s %ld%s\n", fields[i], v,
                   strncmp(fields[i], "Vm", 2) == 0 || strncmp(fields[i], "Rss", 3) == 0 ? " kB"
                                                                                         : "");
    }

    const char *links[] = {"exe", "cwd"};
    for (size_t i = 0; i < ARRAY_LEN(links); i++) {
        snprintf(path, sizeof path, "/proc/%d/%s", (int)pid, links[i]);
        ssize_t n = readlink(path, link, sizeof link - 1);
        if (n >= 0) {
            link[n] = '\0';
            printf("%-10s %s\n", links[i], link);
        } else {
            printf("%-10s (%s)\n", links[i], strerror(errno));
        }
    }

    snprintf(path, sizeof path, "/proc/%d/fd", (int)pid);
    DIR *d = opendir(path);
    if (d) {
        int nfd = 0;
        struct dirent *e;
        while ((e = readdir(d)))
            nfd += e->d_name[0] != '.';
        closedir(d);
        printf("open fds   %d\n", nfd - (pid == getpid())); /* minus our own opendir */
    }
}

static void usage(void)
{
    fprintf(stderr, "usage: llps            list processes\n"
                    "       llps -f         process tree\n"
                    "       llps -t [-i ms] [-n rows] [-c count]   top-like CPU view\n"
                    "       llps -p PID     details of one process\n");
    exit(2);
}

int main(int argc, char **argv)
{
    page_kb = sysconf(_SC_PAGESIZE) / 1024;
    ticks_per_sec = sysconf(_SC_CLK_TCK);

    if (argc == 1) {
        cmd_list();
        return 0;
    }
    if (strcmp(argv[1], "-f") == 0) {
        cmd_tree();
        return 0;
    }
    if (strcmp(argv[1], "-p") == 0 && argc == 3) {
        cmd_detail((pid_t)atoi(argv[2]));
        return 0;
    }
    if (strcmp(argv[1], "-t") == 0) {
        int interval = 1000, rows = 15, count = 1;
        for (int i = 2; i + 1 < argc; i += 2) {
            if (strcmp(argv[i], "-i") == 0)
                interval = atoi(argv[i + 1]);
            else if (strcmp(argv[i], "-n") == 0)
                rows = atoi(argv[i + 1]);
            else if (strcmp(argv[i], "-c") == 0)
                count = atoi(argv[i + 1]);
            else
                usage();
        }
        if (interval < 10 || rows < 1 || count < 1)
            usage();
        cmd_top(interval, rows, count);
        return 0;
    }
    usage();
    return 2;
}
