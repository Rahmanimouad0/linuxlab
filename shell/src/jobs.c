#include "jobs.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "signals.h"
#include "xalloc.h"

/* table[i] holds job %(i+1); a job keeps its number until it is freed. */
static job *table[MAX_JOBS];

job *job_create(const char *text, int background)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!table[i]) {
            job *j = xcalloc(1, sizeof *j);
            j->id = i + 1;
            j->text = xstrdup(text);
            j->background = background;
            table[i] = j;
            return j;
        }
    }
    return NULL;
}

void job_add_process(job *j, pid_t pid)
{
    j->procs = xrealloc(j->procs, (j->nprocs + 1) * sizeof *j->procs);
    j->procs[j->nprocs].pid = pid;
    j->procs[j->nprocs].state = PROC_RUNNING;
    j->procs[j->nprocs].status = 0;
    j->nprocs++;
}

void job_free(job *j)
{
    table[j->id - 1] = NULL;
    free(j->procs);
    free(j->text);
    free(j);
}

job *job_by_id(int id)
{
    if (id < 1 || id > MAX_JOBS)
        return NULL;
    return table[id - 1];
}

job *job_current(void)
{
    /* Prefer the newest stopped job (what the user most likely wants to
     * resume), otherwise the newest job. */
    for (int i = MAX_JOBS - 1; i >= 0; i--)
        if (table[i] && job_is_stopped(table[i]))
            return table[i];
    for (int i = MAX_JOBS - 1; i >= 0; i--)
        if (table[i])
            return table[i];
    return NULL;
}

int job_is_done(const job *j)
{
    for (size_t i = 0; i < j->nprocs; i++)
        if (j->procs[i].state != PROC_DONE)
            return 0;
    return 1;
}

int job_is_stopped(const job *j)
{
    int stopped = 0;
    for (size_t i = 0; i < j->nprocs; i++) {
        if (j->procs[i].state == PROC_RUNNING)
            return 0;
        if (j->procs[i].state == PROC_STOPPED)
            stopped = 1;
    }
    return stopped;
}

int jobs_any_stopped(void)
{
    for (int i = 0; i < MAX_JOBS; i++)
        if (table[i] && job_is_stopped(table[i]))
            return 1;
    return 0;
}

int job_exit_status(const job *j)
{
    if (j->nprocs == 0)
        return 0;
    if (job_is_stopped(j)) {
        for (size_t i = 0; i < j->nprocs; i++)
            if (j->procs[i].state == PROC_STOPPED)
                return 128 + WSTOPSIG(j->procs[i].status);
    }
    /* Like sh without "pipefail": the status of the last command. */
    int st = j->procs[j->nprocs - 1].status;
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    if (WIFSIGNALED(st))
        return 128 + WTERMSIG(st);
    return 0;
}

void job_mark_status(pid_t pid, int status)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        job *j = table[i];
        if (!j)
            continue;
        for (size_t k = 0; k < j->nprocs; k++) {
            process *p = &j->procs[k];
            if (p->pid != pid)
                continue;
            if (WIFSTOPPED(status)) {
                p->state = PROC_STOPPED;
                j->notified = 0;
            } else if (WIFCONTINUED(status)) {
                p->state = PROC_RUNNING;
            } else {
                p->state = PROC_DONE;
            }
            p->status = status;
            return;
        }
    }
    /* A child we don't track (should not happen): already reaped, ignore. */
}

void jobs_reap(void)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0)
        job_mark_status(pid, status);
}

static const char *job_state_text(const job *j, char *buf, size_t len)
{
    if (job_is_stopped(j))
        return "Stopped";
    if (!job_is_done(j))
        return "Running";
    int st = j->procs[j->nprocs - 1].status;
    if (WIFEXITED(st)) {
        if (WEXITSTATUS(st) == 0)
            return "Done";
        snprintf(buf, len, "Exit %d", WEXITSTATUS(st));
        return buf;
    }
    if (WIFSIGNALED(st))
        return strsignal(WTERMSIG(st));
    return "Done";
}

static void print_job(FILE *out, const job *j, int with_pids)
{
    char buf[32];
    char mark = j == job_current() ? '+' : ' ';
    const char *amp = (j->background && !job_is_stopped(j) && !job_is_done(j)) ? " &" : "";
    if (with_pids)
        fprintf(out, "[%d]%c %d %-22s %s%s\n", j->id, mark, (int)j->pgid,
                job_state_text(j, buf, sizeof buf), j->text, amp);
    else
        fprintf(out, "[%d]%c  %-24s%s%s\n", j->id, mark, job_state_text(j, buf, sizeof buf),
                j->text, amp);
}

void jobs_print(FILE *out, int with_pids)
{
    for (int i = 0; i < MAX_JOBS; i++)
        if (table[i])
            print_job(out, table[i], with_pids);
}

void jobs_notify(int print)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        job *j = table[i];
        if (!j)
            continue;
        if (job_is_done(j)) {
            if (print && j->background && !j->notified)
                print_job(stderr, j, 0);
            job_free(j);
        } else if (job_is_stopped(j) && !j->notified) {
            if (print)
                print_job(stderr, j, 0);
            j->notified = 1;
        }
    }
}

int job_wait(job *j, int interruptible)
{
    while (!job_is_done(j) && !job_is_stopped(j)) {
        int status;
        pid_t pid = waitpid(-1, &status, WUNTRACED);
        if (pid < 0) {
            if (errno == EINTR) {
                if (interruptible && g_sigint)
                    return -1;
                continue;
            }
            /* ECHILD: somebody else reaped them; treat as finished. */
            for (size_t k = 0; k < j->nprocs; k++)
                j->procs[k].state = PROC_DONE;
            break;
        }
        /* The pid may belong to another (background) job: record it too. */
        job_mark_status(pid, status);
    }
    return 0;
}

int job_foreground(shell *sh, job *j, int cont)
{
    j->background = 0;
    if (sh->job_control)
        tcsetpgrp(sh->terminal, j->pgid);

    if (cont) {
        if (sh->job_control && j->has_tmodes)
            tcsetattr(sh->terminal, TCSADRAIN, &j->tmodes);
        for (size_t k = 0; k < j->nprocs; k++)
            if (j->procs[k].state == PROC_STOPPED)
                j->procs[k].state = PROC_RUNNING;
        if (kill(-j->pgid, SIGCONT) < 0)
            perror("lsh: kill (SIGCONT)");
    }

    job_wait(j, 0);

    if (sh->job_control) {
        /* Take the terminal back and restore the shell's terminal modes;
         * remember the job's modes in case it was stopped (e.g. an editor). */
        tcsetpgrp(sh->terminal, sh->pgid);
        tcgetattr(sh->terminal, &j->tmodes);
        j->has_tmodes = 1;
        tcsetattr(sh->terminal, TCSADRAIN, &sh->tmodes);
    }

    int status = job_exit_status(j);

    if (job_is_stopped(j)) {
        if (sh->interactive) {
            fputc('\n', stderr);
            print_job(stderr, j, 0);
        }
        j->notified = 1;
    } else {
        if (sh->interactive) {
            int st = j->procs[j->nprocs - 1].status;
            if (WIFSIGNALED(st)) {
                if (WTERMSIG(st) == SIGINT)
                    fputc('\n', stderr);
                else
                    fprintf(stderr, "%s%s\n", strsignal(WTERMSIG(st)),
                            WCOREDUMP(st) ? " (core dumped)" : "");
            }
        }
        job_free(j);
    }
    return status;
}

void job_background(job *j)
{
    for (size_t k = 0; k < j->nprocs; k++)
        if (j->procs[k].state == PROC_STOPPED)
            j->procs[k].state = PROC_RUNNING;
    j->background = 1;
    j->notified = 0;
    if (kill(-j->pgid, SIGCONT) < 0)
        perror("lsh: kill (SIGCONT)");
}
