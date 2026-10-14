/*
 * Signal policy of the shell.
 *
 * Interactive shell:  SIGINT   -> handler that only sets a flag (so Ctrl+C
 *                                 abandons the current input line)
 *                     SIGCHLD  -> handler that only sets a flag; children are
 *                                 reaped synchronously before each prompt
 *                     SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU -> ignored
 * Child processes:    everything reset to SIG_DFL before exec, because
 *                     *ignored* dispositions survive execve() (handled ones
 *                     are reset automatically).
 */
#ifndef LSH_SIGNALS_H
#define LSH_SIGNALS_H

#include <signal.h>

extern volatile sig_atomic_t g_sigint;
extern volatile sig_atomic_t g_sigchld;

void signals_install_shell(void);
void signals_reset_for_child(void);

#endif
