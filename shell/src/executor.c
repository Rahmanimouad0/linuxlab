#include "executor.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "builtins.h"
#include "jobs.h"
#include "signals.h"
#include "xalloc.h"

/* Descriptors saved while a builtin runs inside the shell with redirections. */
typedef struct {
    int fd;    /* the fd that was redirected                        */
    int saved; /* a copy of its previous target, or -1 if it was closed */
} saved_fd;

static int open_target(const redirection *r)
{
    switch (r->type) {
    case REDIR_IN: return open(r->target, O_RDONLY | O_CLOEXEC);
    case REDIR_OUT:
        /* 0666 & ~umask: usually 0644, exactly like other shells. */
        return open(r->target, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    case REDIR_APPEND: return open(r->target, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    default: errno = EINVAL; return -1;
    }
}

/*
 * Apply a command's redirections, left to right.
 *
 *   > out.txt      fd = open("out.txt"); dup2(fd, 1); close(fd);
 *   2>&1           dup2(1, 2);
 *
 * Order matters: "cmd > f 2>&1" sends both streams to f, while
 * "cmd 2>&1 > f" sends stderr to the *old* stdout.
 *
 * If saved is non-NULL the previous state of every touched fd is recorded
 * so it can be restored afterwards (builtins run in the shell itself).
 */
static int apply_redirections(const command *c, saved_fd *saved, size_t *nsaved)
{
    for (size_t i = 0; i < c->nredirs; i++) {
        const redirection *r = &c->redirs[i];

        if (saved) {
            saved[*nsaved].fd = r->fd;
            saved[*nsaved].saved = fcntl(r->fd, F_DUPFD_CLOEXEC, 10);
            (*nsaved)++;
        }

        if (r->type == REDIR_DUP) {
            if (strcmp(r->target, "-") == 0) {
                close(r->fd);
                continue;
            }
            int src = atoi(r->target);
            if (src != r->fd && dup2(src, r->fd) < 0) {
                fprintf(stderr, "lsh: %s: %s\n", r->target, strerror(errno));
                return -1;
            }
            continue;
        }

        int fd = open_target(r);
        if (fd < 0) {
            fprintf(stderr, "lsh: %s: %s\n", r->target, strerror(errno));
            return -1;
        }
        if (fd == r->fd) {
            /* open() returned exactly the fd we wanted: just drop O_CLOEXEC. */
            fcntl(fd, F_SETFD, 0);
        } else {
            if (dup2(fd, r->fd) < 0) {
                fprintf(stderr, "lsh: %s: %s\n", r->target, strerror(errno));
                close(fd);
                return -1;
            }
            close(fd);
        }
    }
    return 0;
}

static void restore_redirections(saved_fd *saved, size_t n)
{
    while (n-- > 0) { /* reverse order undoes repeated redirections of one fd */
        if (saved[n].saved >= 0) {
            dup2(saved[n].saved, saved[n].fd);
            close(saved[n].saved);
        } else {
            close(saved[n].fd);
        }
    }
}

/* A builtin that is not part of a pipeline runs in the shell process. */
static int run_builtin_here(shell *sh, const command *c, builtin_fn fn)
{
    saved_fd *saved = xcalloc(c->nredirs, sizeof *saved);
    size_t n = 0;
    int status;

    fflush(stdout);
    fflush(stderr);
    if (apply_redirections(c, saved, &n) < 0)
        status = 1;
    else
        status = fn(sh, (int)c->argc, c->argv);
    fflush(stdout); /* flush while stdout still points at the redirect target */
    fflush(stderr);
    restore_redirections(saved, n);
    free(saved);
    return status;
}

/*
 * Search $PATH the way a shell does: the first entry holding an executable
 * regular file wins; directories we cannot search are skipped. (execvp()
 * would instead report EACCES if *any* entry was unreadable.)
 * Returns a malloc'd path, or NULL.
 */
static char *find_in_path(const char *name)
{
    const char *path = getenv("PATH");
    if (!path || !*path)
        path = "/usr/local/bin:/usr/bin:/bin";
    size_t nlen = strlen(name);

    for (const char *p = path;;) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        size_t len = dlen + nlen + 2;
        char *full = xmalloc(len);
        if (dlen == 0) /* an empty entry means the current directory */
            snprintf(full, len, "%s", name);
        else
            snprintf(full, len, "%.*s/%s", (int)dlen, p, name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && access(full, X_OK) == 0)
            return full;
        free(full);
        if (!colon)
            return NULL;
        p = colon + 1;
    }
}

/* Runs in the child after fork(); never returns. */
static void child_exec(shell *sh, const command *c, pid_t pgid, int in_fd, int out_fd,
                       int foreground)
{
    if (sh->job_control) {
        pid_t pid = getpid();
        if (pgid == 0)
            pgid = pid;
        setpgid(pid, pgid);
        if (foreground)
            tcsetpgrp(sh->terminal, pgid); /* SIGTTOU is still ignored here */
    }
    signals_reset_for_child();

    if (in_fd != STDIN_FILENO) {
        dup2(in_fd, STDIN_FILENO);
        close(in_fd);
    }
    if (out_fd != STDOUT_FILENO) {
        dup2(out_fd, STDOUT_FILENO);
        close(out_fd);
    }
    if (apply_redirections(c, NULL, NULL) < 0)
        _exit(1);
    if (c->argc == 0) /* e.g. "> file": only the redirection's side effect */
        _exit(0);

    builtin_fn fn = builtin_lookup(c->argv[0]);
    if (fn) { /* a builtin inside a pipeline runs in its own process */
        int status = fn(sh, (int)c->argc, c->argv);
        fflush(NULL);
        _exit(status);
    }

    if (strchr(c->argv[0], '/')) {
        execv(c->argv[0], c->argv);
    } else {
        char *path = find_in_path(c->argv[0]);
        if (!path) {
            fprintf(stderr, "lsh: %s: command not found\n", c->argv[0]);
            _exit(127);
        }
        execv(path, c->argv);
    }

    /* Only reached if exec failed. */
    int err = errno;
    fprintf(stderr, "lsh: %s: %s\n", c->argv[0], strerror(err));
    _exit(err == ENOENT ? 127 : 126);
}

/*
 * Expand the raw words of a command (quotes, $VAR, $?, ~) into out.
 * On failure out may be partially filled; the caller frees it either way.
 */
static int expand_command(shell *sh, const command *raw, command *out)
{
    lex_ctx ctx = {sh->last_status, (long)getpid()};
    char err[128];

    out->argv = xcalloc(raw->argc + 1, sizeof *out->argv);
    for (size_t i = 0; i < raw->argc; i++) {
        char *w;
        if (expand_word(raw->argv[i], &ctx, &w, err, sizeof err) < 0) {
            fprintf(stderr, "lsh: %s\n", err);
            return -1;
        }
        /* An empty unquoted expansion gives no argument. argv has room for
         * raw->argc + 1 entries; the analyzer only objects because that sum
         * could in theory wrap around, which no real command line can reach. */
        if (w && out->argc < raw->argc)
            out->argv[out->argc++] = w; // NOLINT(clang-analyzer-security.ArrayBound)
    }

    out->redirs = xcalloc(raw->nredirs, sizeof *out->redirs);
    for (size_t i = 0; i < raw->nredirs; i++) {
        char *w;
        if (expand_word(raw->redirs[i].target, &ctx, &w, err, sizeof err) < 0) {
            fprintf(stderr, "lsh: %s\n", err);
            return -1;
        }
        if (!w) {
            fprintf(stderr, "lsh: %s: ambiguous redirect\n", raw->redirs[i].target);
            return -1;
        }
        out->redirs[i] = raw->redirs[i];
        out->redirs[i].target = w;
        out->nredirs = i + 1;
    }
    return 0;
}

static int run_pipeline(shell *sh, const pipeline *p, const command *cmds)
{
    if (p->ncmds == 1 && !p->background && cmds[0].argc > 0) {
        builtin_fn fn = builtin_lookup(cmds[0].argv[0]);
        if (fn)
            return run_builtin_here(sh, &cmds[0], fn);
    }

    job *j = job_create(p->text, p->background);
    if (!j) {
        fprintf(stderr, "lsh: too many jobs\n");
        return 1;
    }

    int in_fd = STDIN_FILENO;
    if (p->background && !sh->job_control) {
        /* Without job control a background job must not steal our input. */
        in_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (in_fd < 0)
            in_fd = STDIN_FILENO;
    }

    pid_t last_pid = -1;
    for (size_t i = 0; i < p->ncmds; i++) {
        int fds[2] = {-1, -1};
        int out_fd = STDOUT_FILENO;

        if (i + 1 < p->ncmds) {
            if (pipe2(fds, O_CLOEXEC) < 0) {
                perror("lsh: pipe");
                break;
            }
            out_fd = fds[1];
        }

        fflush(NULL); /* otherwise buffered output would be duplicated in the child */
        pid_t pid = fork();
        if (pid < 0) {
            perror("lsh: fork");
            if (fds[0] >= 0) {
                close(fds[0]);
                close(fds[1]);
            }
            break;
        }
        if (pid == 0) {
            if (fds[0] >= 0)
                close(fds[0]); /* the read end belongs to the next command */
            child_exec(sh, &cmds[i], j->pgid, in_fd, out_fd, !p->background);
        }

        /* Parent. Call setpgid here too: whichever of parent/child runs
         * first, the group exists before anyone relies on it. */
        if (sh->job_control) {
            if (j->pgid == 0)
                j->pgid = pid;
            setpgid(pid, j->pgid);
        }
        job_add_process(j, pid);
        last_pid = pid;

        if (in_fd != STDIN_FILENO)
            close(in_fd);
        if (out_fd != STDOUT_FILENO)
            close(out_fd);
        in_fd = fds[0];
    }
    if (in_fd != STDIN_FILENO && in_fd >= 0)
        close(in_fd);

    if (j->nprocs == 0) {
        job_free(j);
        return 1;
    }

    if (p->background) {
        if (sh->interactive)
            fprintf(stderr, "[%d] %d\n", j->id, (int)last_pid);
        return 0;
    }
    return job_foreground(sh, j, 0);
}

int exec_pipeline(shell *sh, const pipeline *p)
{
    command *cmds = xcalloc(p->ncmds, sizeof *cmds);
    int status = 1;

    size_t i;
    for (i = 0; i < p->ncmds; i++)
        if (expand_command(sh, &p->cmds[i], &cmds[i]) < 0)
            break;
    if (i == p->ncmds)
        status = run_pipeline(sh, p, cmds);

    for (i = 0; i < p->ncmds; i++)
        command_free(&cmds[i]);
    free(cmds);
    return status;
}

void exec_list(shell *sh, const cmd_list *l)
{
    for (size_t i = 0; i < l->n; i++) {
        if (i > 0) {
            next_op prev = l->items[i - 1].next;
            if (prev == NEXT_AND && sh->last_status != 0)
                continue;
            if (prev == NEXT_OR && sh->last_status == 0)
                continue;
        }
        sh->last_status = exec_pipeline(sh, &l->items[i].pl);
        if (sh->exit_requested)
            break;
    }
}
