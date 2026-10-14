/*
 * Allocation wrappers. A shell has no sensible way to recover from running
 * out of memory in the middle of parsing a line, so we abort loudly instead
 * of threading error codes through every function.
 */
#ifndef LSH_XALLOC_H
#define LSH_XALLOC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void *xoom(void *p)
{
    if (!p) {
        fputs("lsh: out of memory\n", stderr);
        abort();
    }
    return p;
}

static inline void *xmalloc(size_t n) { return xoom(malloc(n ? n : 1)); }
static inline void *xcalloc(size_t n, size_t sz) { return xoom(calloc(n ? n : 1, sz ? sz : 1)); }
static inline void *xrealloc(void *p, size_t n) { return xoom(realloc(p, n ? n : 1)); }
static inline char *xstrdup(const char *s) { return xoom(strdup(s)); }
static inline char *xstrndup(const char *s, size_t n) { return xoom(strndup(s, n)); }

#endif
