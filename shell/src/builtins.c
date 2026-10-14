#include "builtins.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "jobs.h"
#include "xalloc.h"

extern char **environ;

/* ---------------------------------------------------------------- helpers */

static job *parse_jobspec(const char *who, const char *arg)
{
    job *j = NULL;
    if (!arg || strcmp(arg, "%%") == 0 || strcmp(arg, "%+") == 0 || strcmp(arg, "%") == 0) {
        j = job_current();
    } else {
        const char *s = arg[0] == '%' ? arg + 1 : arg;
        char *end;
        long id = strtol(s, &end, 10);
        if (*s && !*end)
            j = job_by_id((int)id);
    }
    if (!j)
        fprintf(stderr, "lsh: %s: %s: no such job\n", who, arg ? arg : "current");
    return j;
}

static const struct {
    const char *name;
    int sig;
} signal_names[] = {
    {"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT}, {"KILL", SIGKILL}, {"USR1", SIGUSR1},
    {"USR2", SIGUSR2}, {"PIPE", SIGPIPE}, {"ALRM", SIGALRM}, {"TERM", SIGTERM}, {"CHLD", SIGCHLD},
    {"CONT", SIGCONT}, {"STOP", SIGSTOP}, {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN}, {"TTOU", SIGTTOU},
};

static int parse_signal(const char *s)
{
    if (isdigit((unsigned char)*s)) {
        char *end;
        long n = strtol(s, &end, 10);
        return (*end || n < 0 || n >= NSIG) ? -1 : (int)n;
    }
    if (strncasecmp(s, "SIG", 3) == 0)
        s += 3;
    for (size_t i = 0; i < sizeof signal_names / sizeof signal_names[0]; i++)
        if (strcasecmp(s, signal_names[i].name) == 0)
            return signal_names[i].sig;
    return -1;
}

static int valid_name(const char *s)
{
    if (!(isalpha((unsigned char)*s) || *s == '_'))
        return 0;
    for (; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '_'))
            return 0;
    return 1;
}

/* --------------------------------------------------------------- builtins */

static int bi_cd(shell *sh, int argc, char **argv)
{
    (void)sh;
    if (argc > 2) {
        fprintf(stderr, "lsh: cd: too many arguments\n");
        return 1;
    }
    const char *target = argc > 1 ? argv[1] : getenv("HOME");
    int print = 0;
    if (!target) {
        fprintf(stderr, "lsh: cd: HOME not set\n");
        return 1;
    }
    if (strcmp(target, "-") == 0) {
        target = getenv("OLDPWD");
        if (!target) {
            fprintf(stderr, "lsh: cd: OLDPWD not set\n");
            return 1;
        }
        print = 1;
    }

    char old[PATH_MAX];
    int have_old = getcwd(old, sizeof old) != NULL;
    if (chdir(target) < 0) {
        fprintf(stderr, "lsh: cd: %s: %s\n", target, strerror(errno));
        return 1;
    }

    char cur[PATH_MAX];
    if (have_old)
        setenv("OLDPWD", old, 1);
    if (getcwd(cur, sizeof cur)) {
        setenv("PWD", cur, 1);
        if (print)
            printf("%s\n", cur);
    }
    return 0;
}

static int bi_pwd(shell *sh, int argc, char **argv)
{
    (void)sh, (void)argc, (void)argv;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) {
        fprintf(stderr, "lsh: pwd: %s\n", strerror(errno));
        return 1;
    }
    printf("%s\n", cwd);
    return 0;
}

static int bi_exit(shell *sh, int argc, char **argv)
{
    int code = sh->last_status;
    if (argc > 2) {
        fprintf(stderr, "lsh: exit: too many arguments\n");
        return 1;
    }
    if (argc == 2) {
        char *end;
        long v = strtol(argv[1], &end, 10);
        if (!*argv[1] || *end) {
            fprintf(stderr, "lsh: exit: %s: numeric argument required\n", argv[1]);
            code = 2;
        } else {
            code = (int)(v & 0xff); /* only the low 8 bits reach the parent */
        }
    }
    if (sh->job_control && !sh->warned_stopped && jobs_any_stopped()) {
        fprintf(stderr, "There are stopped jobs.\n");
        sh->warned_stopped = 1;
        return 1;
    }
    sh->exit_requested = 1;
    sh->exit_code = code;
    return code;
}

static int bi_export(shell *sh, int argc, char **argv)
{
    (void)sh;
    if (argc == 1) {
        for (char **e = environ; *e; e++)
            printf("export %s\n", *e);
        return 0;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        char *name = eq ? xstrndup(argv[i], (size_t)(eq - argv[i])) : xstrdup(argv[i]);
        if (!valid_name(name)) {
            fprintf(stderr, "lsh: export: `%s': not a valid identifier\n", argv[i]);
            rc = 1;
        } else if (eq) {
            setenv(name, eq + 1, 1);
        }
        free(name);
    }
    return rc;
}

static int bi_unset(shell *sh, int argc, char **argv)
{
    (void)sh;
    for (int i = 1; i < argc; i++)
        unsetenv(argv[i]);
    return 0;
}

static int bi_jobs(shell *sh, int argc, char **argv)
{
    (void)sh;
    int with_pids = argc > 1 && strcmp(argv[1], "-l") == 0;
    jobs_print(stdout, with_pids);
    return 0;
}

static int bi_fg(shell *sh, int argc, char **argv)
{
    if (!sh->job_control) {
        fprintf(stderr, "lsh: fg: no job control\n");
        return 1;
    }
    job *j = parse_jobspec("fg", argc > 1 ? argv[1] : NULL);
    if (!j)
        return 1;
    printf("%s\n", j->text);
    fflush(stdout);
    return job_foreground(sh, j, 1);
}

static int bi_bg(shell *sh, int argc, char **argv)
{
    if (!sh->job_control) {
        fprintf(stderr, "lsh: bg: no job control\n");
        return 1;
    }
    job *j = parse_jobspec("bg", argc > 1 ? argv[1] : NULL);
    if (!j)
        return 1;
    if (!job_is_stopped(j)) {
        fprintf(stderr, "lsh: bg: job %d already in background\n", j->id);
        return 0;
    }
    job_background(j);
    printf("[%d]+ %s &\n", j->id, j->text);
    return 0;
}

static int bi_wait(shell *sh, int argc, char **argv)
{
    (void)sh;
    if (argc == 1) {
        for (int id = 1; id <= MAX_JOBS; id++) {
            job *j = job_by_id(id);
            if (j && !job_is_stopped(j)) {
                if (job_wait(j, 1) < 0)
                    return 130;
                j->notified = 1;
            }
        }
        return 0;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        job *j = parse_jobspec("wait", argv[i]);
        if (!j) {
            status = 127;
            continue;
        }
        if (job_wait(j, 1) < 0)
            return 130;
        j->notified = 1;
        status = job_exit_status(j);
    }
    return status;
}

static int bi_kill(shell *sh, int argc, char **argv)
{
    (void)sh;
    int sig = SIGTERM, i = 1;

    if (argc > 1 && strcmp(argv[1], "-l") == 0) {
        for (size_t k = 0; k < sizeof signal_names / sizeof signal_names[0]; k++)
            printf("%2d) SIG%s\n", signal_names[k].sig, signal_names[k].name);
        return 0;
    }
    if (argc > 1 && argv[1][0] == '-' && argv[1][1]) {
        sig = parse_signal(argv[1] + 1);
        if (sig < 0) {
            fprintf(stderr, "lsh: kill: %s: invalid signal specification\n", argv[1] + 1);
            return 1;
        }
        i = 2;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: kill [-SIGNAL] %%job | pid ...\n");
        return 2;
    }

    int rc = 0;
    for (; i < argc; i++) {
        if (argv[i][0] == '%') {
            job *j = parse_jobspec("kill", argv[i]);
            if (!j) {
                rc = 1;
                continue;
            }
            int stopped = job_is_stopped(j);
            if (j->pgid > 0) {
                if (kill(-j->pgid, sig) < 0)
                    rc = 1;
                /* A stopped process only acts on most signals once continued. */
                if (stopped && sig != SIGKILL && sig != SIGCONT)
                    kill(-j->pgid, SIGCONT);
            } else {
                for (size_t k = 0; k < j->nprocs; k++)
                    if (j->procs[k].state != PROC_DONE && kill(j->procs[k].pid, sig) < 0)
                        rc = 1;
            }
        } else {
            char *end;
            long pid = strtol(argv[i], &end, 10);
            if (!*argv[i] || *end) {
                fprintf(stderr, "lsh: kill: %s: arguments must be process or job IDs\n", argv[i]);
                rc = 1;
            } else if (kill((pid_t)pid, sig) < 0) {
                fprintf(stderr, "lsh: kill: (%ld) - %s\n", pid, strerror(errno));
                rc = 1;
            }
        }
    }
    return rc;
}

static int bi_help(shell *sh, int argc, char **argv);

static const struct {
    const char *name;
    builtin_fn fn;
    const char *usage;
} builtins[] = {
    {"cd", bi_cd, "cd [dir | -]        change the shell's working directory"},
    {"pwd", bi_pwd, "pwd                 print the working directory"},
    {"exit", bi_exit, "exit [n]            leave the shell with status n"},
    {"export", bi_export, "export [NAME=VALUE] set an environment variable"},
    {"unset", bi_unset, "unset NAME...       remove environment variables"},
    {"jobs", bi_jobs, "jobs [-l]           list jobs"},
    {"fg", bi_fg, "fg [%n]             resume a job in the foreground"},
    {"bg", bi_bg, "bg [%n]             resume a stopped job in the background"},
    {"wait", bi_wait, "wait [%n...]        wait for background jobs"},
    {"kill", bi_kill, "kill [-SIG] %n|pid  send a signal (kill -l lists names)"},
    {"help", bi_help, "help                show this help"},
};

static int bi_help(shell *sh, int argc, char **argv)
{
    (void)sh, (void)argc, (void)argv;
    printf("LinuxLab shell (lsh) - builtins:\n");
    for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; i++)
        printf("  %s\n", builtins[i].usage);
    printf("\nSyntax:\n"
           "  a | b | c           pipeline\n"
           "  < f  > f  >> f      redirect stdin / stdout (truncate, append)\n"
           "  2> f  2>&1  n>&-    redirect / duplicate / close any fd\n"
           "  a ; b   a && b   a || b   a &\n"
           "  'literal'  \"$VAR ${VAR} $? $$\"  \\x  ~\n"
           "\nCtrl+C interrupts, Ctrl+Z stops the foreground job.\n");
    return 0;
}

builtin_fn builtin_lookup(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; i++)
        if (strcmp(name, builtins[i].name) == 0)
            return builtins[i].fn;
    return NULL;
}
