#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void die(const char *fmt, ...)
{
    va_list ap;
    fputs("error: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

void die_errno(const char *what)
{
    fprintf(stderr, "error: %s: %s\n", what, strerror(errno));
    exit(EXIT_FAILURE);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        die("out of memory (malloc %zu bytes)", n);
    return p;
}

void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p)
        die("out of memory (calloc %zu x %zu bytes)", n, size);
    return p;
}

void section(const char *title)
{
    printf("\n== %s ==\n", title);
    fflush(stdout);
}

uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void sleep_ms(long ms)
{
    struct timespec req = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&req, &req) < 0 && errno == EINTR) {
    }
}

ssize_t read_file(const char *path, char *buf, size_t len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off + 1 < len) {
        ssize_t n = read(fd, buf + off, len - 1 - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        off += (size_t)n;
    }
    buf[off] = '\0';
    close(fd);
    return (ssize_t)off;
}

char proc_state(pid_t pid)
{
    char path[64], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    if (read_file(path, buf, sizeof buf) <= 0)
        return 0;
    /* The command name is in parentheses and may itself contain spaces or
     * parentheses, so the state is found after the *last* ')'. */
    const char *p = strrchr(buf, ')');
    if (!p || p[1] == '\0' || p[2] == '\0')
        return 0;
    return p[2];
}

const char *proc_state_name(char state)
{
    switch (state) {
    case 'R': return "running";
    case 'S': return "sleeping";
    case 'D': return "disk sleep";
    case 'T': return "stopped";
    case 't': return "tracing stop";
    case 'Z': return "zombie";
    case 'X': return "dead";
    case 'I': return "idle";
    case 0: return "gone";
    default: return "unknown";
    }
}

long proc_status_field(pid_t pid, const char *field)
{
    char path[64], buf[8192];
    if (pid == 0)
        snprintf(path, sizeof path, "/proc/self/status");
    else
        snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
    if (read_file(path, buf, sizeof buf) <= 0)
        return -1;

    size_t flen = strlen(field);
    for (const char *line = buf; line && *line;) {
        if (strncmp(line, field, flen) == 0 && line[flen] == ':')
            return strtol(line + flen + 1, NULL, 10);
        line = strchr(line, '\n');
        if (line)
            line++;
    }
    return -1;
}
