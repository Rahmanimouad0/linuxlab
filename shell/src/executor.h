/*
 * Executor: turns the parsed AST into processes.
 *
 *   for each command in the pipeline:
 *       pipe()                       (except for the last command)
 *       fork()
 *         child:  setpgid, dup2 pipe ends onto 0/1, apply redirections,
 *                 reset signals, execvp()
 *         parent: setpgid (again, to win the race), close pipe ends
 *   foreground: hand the terminal to the job and waitpid() until done/stopped
 *   background: record the job and return immediately
 */
#ifndef LSH_EXECUTOR_H
#define LSH_EXECUTOR_H

#include "parser.h"
#include "shell.h"

/* Run one pipeline; returns its exit status. */
int exec_pipeline(shell *sh, const pipeline *p);

/* Run a whole parsed line honouring ; && || and &. Updates sh->last_status. */
void exec_list(shell *sh, const cmd_list *l);

#endif
