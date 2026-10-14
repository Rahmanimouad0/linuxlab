/*
 * Small helpers shared by the labs, tools and benchmarks.
 * The shell (shell/) deliberately does not depend on this file.
 */
#ifndef LINUXLAB_UTIL_H
#define LINUXLAB_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Print "error: ..." to stderr and exit(1). */
void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
/* Print "error: <what>: strerror(errno)" and exit(1). */
void die_errno(const char *what) __attribute__((noreturn));

/*
 * Tell the compiler that *p may be read or written by code it cannot see.
 * Without this, GCC is allowed to delete e.g. malloc + memset + free
 * entirely, which would make memory experiments measure nothing.
 */
static inline void escape(void *p) { __asm__ volatile("" : : "g"(p) : "memory"); }

/* malloc/calloc that exit with an error message instead of returning NULL. */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);

/* Print a section header for lab output. */
void section(const char *title);

/* Monotonic clock in nanoseconds. */
uint64_t now_ns(void);

/* Sleep for the given number of milliseconds (restarts on EINTR). */
void sleep_ms(long ms);

/* Read a whole (small) file into buf, NUL-terminated. Returns bytes read or -1. */
ssize_t read_file(const char *path, char *buf, size_t len);

/* The one-letter state of a process from /proc/<pid>/stat, or 0 if it is gone. */
char proc_state(pid_t pid);

/* Human-readable name for a /proc state letter. */
const char *proc_state_name(char state);

/*
 * First number on the "<field>:" line of /proc/<pid>/status
 * (pid 0 means "self"). Returns -1 if the field is missing.
 * Example: proc_status_field(0, "VmRSS") -> resident set size in kB.
 */
long proc_status_field(pid_t pid, const char *field);

#endif
