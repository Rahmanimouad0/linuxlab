/*
 * lsh - the LinuxLab shell.
 *
 *   lsh               interactive (job control enabled when on a terminal)
 *   lsh -c 'cmd'      run one command line
 *   lsh script.sh     run a script
 *
 * The main loop is the classic read -> parse -> execute cycle; before each
 * prompt, children that changed state are reaped and reported.
 */
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "executor.h"
#include "jobs.h"
#include "parser.h"
#include "shell.h"
#include "signals.h"

#define LSH_VERSION "1.0.0"

static void usage(FILE *out)
{
    fprintf(out, "usage: lsh [-c command | script]\n"
                 "LinuxLab shell " LSH_VERSION " - type 'help' inside the shell\n");
}

/* Put the shell in its own process group and take the terminal (glibc manual). */
static void init_job_control(shell *sh)
{
    sh->terminal = STDIN_FILENO;

    /* If we were started in the background, wait until we are foreground. */
    while (tcgetpgrp(sh->terminal) != (sh->pgid = getpgrp()))
        kill(-sh->pgid, SIGTTIN);

    signals_install_shell();

    sh->pgid = getpid();
    if (getpgrp() != sh->pgid && setpgid(sh->pgid, sh->pgid) < 0) {
        perror("lsh: setpgid");
        return; /* run without job control */
    }
    tcsetpgrp(sh->terminal, sh->pgid);
    tcgetattr(sh->terminal, &sh->tmodes);
    sh->job_control = 1;
}

static void print_prompt(void)
{
    char cwd[PATH_MAX];
    const char *home = getenv("HOME");
    if (!getcwd(cwd, sizeof cwd))
        strcpy(cwd, "?");

    const char *shown = cwd;
    const char *tilde = "";
    size_t hl = home ? strlen(home) : 0;
    if (hl > 1 && strncmp(cwd, home, hl) == 0 && (cwd[hl] == '/' || cwd[hl] == '\0')) {
        shown = cwd + hl;
        tilde = "~";
    }
    fprintf(stderr, "\033[1;32mlinuxlab\033[0m:\033[1;34m%s%s\033[0m$ ", tilde, shown);
    fflush(stderr);
}

static void run_line(shell *sh, const char *line)
{
    char err[256];
    cmd_list l;

    if (parse_line(line, &l, err, sizeof err) < 0) {
        fprintf(stderr, "lsh: %s\n", err);
        sh->last_status = 2;
        return;
    }
    exec_list(sh, &l);
    cmd_list_free(&l);
}

int main(int argc, char **argv)
{
    shell sh;
    memset(&sh, 0, sizeof sh);
    sh.terminal = STDIN_FILENO;

    FILE *input = stdin;
    FILE *script = NULL;
    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0) {
            printf("lsh " LSH_VERSION "\n");
            return 0;
        }
        if (strcmp(argv[1], "-c") == 0) {
            if (argc < 3) {
                usage(stderr);
                return 2;
            }
            run_line(&sh, argv[2]);
            return sh.exit_requested ? sh.exit_code : sh.last_status;
        }
        script = fopen(argv[1], "re"); /* "e": O_CLOEXEC, children must not inherit it */
        if (!script) {
            fprintf(stderr, "lsh: %s: %s\n", argv[1], strerror(errno));
            return 127;
        }
        input = script;
    }

    sh.interactive = input == stdin && isatty(STDIN_FILENO) && isatty(STDERR_FILENO);
    if (sh.interactive) {
        init_job_control(&sh);
        fprintf(stderr, "LinuxLab shell " LSH_VERSION " - type 'help' for builtins, "
                        "Ctrl+D to exit\n");
    }

    char *line = NULL;
    size_t cap = 0;
    for (;;) {
        /* Without the SIGCHLD handler (non-interactive) just poll every line. */
        if (!sh.interactive || g_sigchld) {
            g_sigchld = 0;
            jobs_reap();
        }
        jobs_notify(sh.interactive);

        if (sh.interactive)
            print_prompt();
        g_sigint = 0;
        errno = 0;

        ssize_t n = getline(&line, &cap, input);
        if (n < 0) {
            if (sh.interactive && errno == EINTR) { /* Ctrl+C at the prompt */
                clearerr(input);
                fputc('\n', stderr);
                sh.last_status = 130;
                continue;
            }
            if (sh.interactive)
                fputs("exit\n", stderr);
            break;
        }
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';

        run_line(&sh, line);
        if (sh.exit_requested)
            break;
    }

    free(line);
    if (script)
        fclose(script);
    return sh.exit_requested ? sh.exit_code : sh.last_status;
}
