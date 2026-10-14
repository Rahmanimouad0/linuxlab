/*
 * fifo_lab - named pipes: a pipe with a name in the filesystem, so that
 * *unrelated* processes can find it.
 *
 *   Process A ──write──▶ /tmp/linuxlab-fifo-<pid> ──read──▶ Process B
 *
 *   1. the FIFO is a file of type 'p' but stores no data on disk
 *   2. open() blocks until both a reader and a writer exist (rendezvous)
 *   3. data flows exactly like through an anonymous pipe
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    char path[64];
    snprintf(path, sizeof path, "/tmp/linuxlab-fifo-%d", (int)getpid());
    if (mkfifo(path, 0600) < 0)
        die_errno("mkfifo");

    section("1. a FIFO in the filesystem");
    struct stat st;
    if (stat(path, &st) < 0)
        die_errno("stat");
    printf("  %s: S_ISFIFO=%d, size=%lld bytes\n", path, S_ISFIFO(st.st_mode) ? 1 : 0,
           (long long)st.st_size);
    printf("  (ls -l shows it as: p%c%c%c%c%c%c%c%c%c)\n", st.st_mode & 0400 ? 'r' : '-',
           st.st_mode & 0200 ? 'w' : '-', st.st_mode & 0100 ? 'x' : '-',
           st.st_mode & 040 ? 'r' : '-', st.st_mode & 020 ? 'w' : '-', st.st_mode & 010 ? 'x' : '-',
           st.st_mode & 04 ? 'r' : '-', st.st_mode & 02 ? 'w' : '-', st.st_mode & 01 ? 'x' : '-');

    section("2. open() is a rendezvous");
    uint64_t t0 = now_ns();
    pid_t writer = fork();
    if (writer < 0)
        die_errno("fork");
    if (writer == 0) {
        uint64_t before = now_ns();
        int fd = open(path, O_WRONLY); /* blocks until a reader opens */
        printf("  writer: open(O_WRONLY) blocked for %.0f ms\n", (now_ns() - before) / 1e6);
        const char *lines[] = {"first message\n", "second message\n", "third message\n"};
        for (size_t i = 0; i < ARRAY_LEN(lines); i++)
            if (write(fd, lines[i], strlen(lines[i])) < 0)
                _exit(1);
        close(fd);
        _exit(0);
    }

    sleep_ms(300); /* the writer is now stuck in open() */
    printf("  reader: opening after %.0f ms\n", (now_ns() - t0) / 1e6);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        die_errno("open fifo");

    section("3. data flow");
    FILE *in = fdopen(fd, "r");
    char line[128];
    while (fgets(line, sizeof line, in))
        printf("  reader got: %s", line);
    fclose(in);
    waitpid(writer, NULL, 0);

    if (stat(path, &st) == 0)
        printf("  after the transfer the FIFO still has size %lld: data never touched disk\n",
               (long long)st.st_size);
    unlink(path);
    printf("use a FIFO when two independent programs (e.g. started from different\n"
           "shells) need a simple one-way byte stream.\n");
    return 0;
}
