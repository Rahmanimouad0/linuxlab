/*
 * Job table.
 *
 * A job is one pipeline. All processes of a job share a process group
 * (pgid = pid of the first process) when job control is enabled, so that
 * the terminal driver can deliver Ctrl+C / Ctrl+Z to the whole pipeline
 * and nothing else.
 */
#ifndef LSH_JOBS_H
#define LSH_JOBS_H

#include <stdio.h>
#include <sys/types.h>
#include <termios.h>

#include "shell.h"

#define MAX_JOBS 64

typedef enum { PROC_RUNNING, PROC_STOPPED, PROC_DONE } proc_state;

typedef struct {
    pid_t pid;
    proc_state state;
    int status; /* raw status from waitpid() */
} process;

typedef struct job {
    int id; /* %1, %2, ... */
    pid_t pgid;
    char *text;
    process *procs;
    size_t nprocs;
    int background;
    int notified;          /* the user has seen the job's current state    */
    struct termios tmodes; /* terminal modes saved when the job was stopped */
    int has_tmodes;
} job;

job *job_create(const char *text, int background);
void job_add_process(job *j, pid_t pid);
void job_free(job *j);

job *job_by_id(int id);
job *job_current(void); /* the job "fg"/"bg" act on without an argument */

int job_is_done(const job *j);
int job_is_stopped(const job *j);
int jobs_any_stopped(void);

/* Exit status of a job as seen by $?: exit code, or 128 + signal number. */
int job_exit_status(const job *j);

/* Record a status change reported by waitpid(). */
void job_mark_status(pid_t pid, int status);

/* Collect every child state change without blocking. */
void jobs_reap(void);

/* Print "Done" / "Stopped" notifications and drop finished jobs. */
void jobs_notify(int print);

/* The "jobs" builtin. */
void jobs_print(FILE *out, int with_pids);

/*
 * Block until the job finishes or stops. With interruptible set, a SIGINT
 * delivered to the shell aborts the wait and returns -1.
 */
int job_wait(job *j, int interruptible);

/* Give the terminal to the job, optionally SIGCONT it, wait, take it back. */
int job_foreground(shell *sh, job *j, int cont);

/* Let a stopped job continue in the background. */
void job_background(job *j);

#endif
