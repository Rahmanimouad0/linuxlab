/* Global state of one running shell instance. */
#ifndef LSH_SHELL_H
#define LSH_SHELL_H

#include <sys/types.h>
#include <termios.h>

typedef struct shell {
    int interactive;       /* commands come from a terminal                     */
    int job_control;       /* process groups + terminal hand-off are enabled    */
    int terminal;          /* fd of the controlling terminal                    */
    pid_t pgid;            /* the shell's own process group                     */
    struct termios tmodes; /* terminal modes restored after each foreground job */
    int last_status;       /* $?                                                */
    int exit_requested;    /* set by the exit builtin                           */
    int exit_code;
    int warned_stopped; /* "There are stopped jobs." already printed once    */
} shell;

#endif
