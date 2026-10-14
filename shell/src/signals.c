#include "signals.h"

#include <string.h>

volatile sig_atomic_t g_sigint = 0;
volatile sig_atomic_t g_sigchld = 0;

/* Handlers do the minimum that is async-signal-safe: set a flag. */
static void on_sigint(int sig)
{
    (void)sig;
    g_sigint = 1;
}

static void on_sigchld(int sig)
{
    (void)sig;
    g_sigchld = 1;
}

static void set_handler(int sig, void (*handler)(int), int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

void signals_install_shell(void)
{
    /* No SA_RESTART: Ctrl+C must interrupt the blocking read of the prompt. */
    set_handler(SIGINT, on_sigint, 0);
    set_handler(SIGCHLD, on_sigchld, SA_RESTART);

    set_handler(SIGQUIT, SIG_IGN, 0);
    set_handler(SIGTSTP, SIG_IGN, 0); /* Ctrl+Z must not stop the shell itself    */
    set_handler(SIGTTIN, SIG_IGN, 0); /* reading/ writing the terminal while not  */
    set_handler(SIGTTOU, SIG_IGN, 0); /* the foreground group (e.g. tcsetpgrp)    */
}

void signals_reset_for_child(void)
{
    static const int sigs[] = {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGCHLD, SIGPIPE};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        set_handler(sigs[i], SIG_DFL, 0);

    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
}
