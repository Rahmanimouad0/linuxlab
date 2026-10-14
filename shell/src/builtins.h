/*
 * Builtins run inside the shell process itself, because their whole point is
 * to change the shell's own state (cwd, environment, job table, exit).
 * Running "cd" in a forked child would change the child's directory and
 * then vanish with it.
 */
#ifndef LSH_BUILTINS_H
#define LSH_BUILTINS_H

#include "shell.h"

typedef int (*builtin_fn)(shell *sh, int argc, char **argv);

/* NULL if name is not a builtin. */
builtin_fn builtin_lookup(const char *name);

#endif
