/*
 * mem_lab - virtual memory as seen through /proc.
 *
 *   1. allocation is lazy: VmSize grows at malloc(), VmRSS only when touched
 *   2. malloc: small blocks come from the heap, large ones from mmap()
 *   3. mapping a file adds a line to /proc/self/maps
 *   4. fork() + copy-on-write, counted in page faults
 *
 * usage: mem_lab [megabytes]   (default 256)
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

static long page_size;

static long minor_faults(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_minflt;
}

static void mem_line(const char *label)
{
    printf("  %-34s VmSize %8ld kB   VmRSS %8ld kB\n", label, proc_status_field(0, "VmSize"),
           proc_status_field(0, "VmRSS"));
}

static void exp_lazy(size_t mb)
{
    section("1. allocation is lazy");
    size_t bytes = mb << 20;
    mem_line("start");

    char *p = malloc(bytes);
    if (!p)
        die("malloc(%zu MB) failed", mb);
    escape(p);
    mem_line("after malloc");

    long f0 = minor_faults();
    memset(p, 1, bytes / 2);
    escape(p);
    long f1 = minor_faults();
    mem_line("after writing the first half");

    memset(p + bytes / 2, 1, bytes - bytes / 2);
    escape(p);
    mem_line("after writing everything");

    long f2 = minor_faults();
    memset(p, 2, bytes);
    escape(p);
    long f3 = minor_faults();

    printf("  page faults: first touch of %zu MB: %ld, second touch of all %zu MB: %ld\n", mb / 2,
           f1 - f0, mb, f3 - f2);
    printf("  (page size %ld bytes -> %zu pages in %zu MB; fewer faults than pages\n"
           "   means the kernel used transparent huge pages or fault-around)\n",
           page_size, (mb / 2 << 20) / (size_t)page_size, mb / 2);

    free(p);
    mem_line("after free");
    printf("malloc() only reserves address space. Physical pages are allocated\n"
           "one page fault at a time, on first write. A large free() is returned\n"
           "to the kernel immediately (munmap), so VmSize drops again.\n");
}

/* Print the /proc/self/maps line that contains addr. */
static void print_region(const char *label, const void *addr)
{
    char buf[1 << 16];
    if (read_file("/proc/self/maps", buf, sizeof buf) < 0)
        die_errno("/proc/self/maps");
    unsigned long a = (unsigned long)addr;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        unsigned long lo, hi;
        if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && a >= lo && a < hi) {
            const char *name = strrchr(line, ' ');
            printf("  %-18s %p  in  %lx-%lx %s\n", label, addr, lo, hi,
                   name && name[1] ? name + 1 : "[anonymous mapping]");
            return;
        }
    }
    printf("  %-18s %p  (not found)\n", label, addr);
}

static void exp_malloc_sources(void)
{
    section("2. where malloc() gets memory");
    void *brk_before = sbrk(0);
    void *small = malloc(64);
    void *large = malloc(4 << 20);
    if (!small || !large)
        die("malloc failed");
    escape(small);
    escape(large);
    void *brk_after = sbrk(0);
    int local = 0;

    print_region("malloc(64)", small);
    print_region("malloc(4 MB)", large);
    print_region("stack variable", &local);
    print_region("a function's code", (void *)(uintptr_t)&print_region);
    printf("  program break: %p -> %p\n", brk_before, brk_after);
    printf("glibc serves small requests from the [heap] (grown with brk) and large\n"
           "ones (>= M_MMAP_THRESHOLD, 128 KiB by default) with a private mmap().\n");
    free(small);
    free(large);
}

static int count_lines(const char *path)
{
    static char buf[1 << 16];
    if (read_file(path, buf, sizeof buf) < 0)
        return -1;
    int n = 0;
    for (char *p = buf; *p; p++)
        n += *p == '\n';
    return n;
}

static void exp_file_mapping(void)
{
    section("3. mapping a file");
    char path[] = "/tmp/linuxlab-mem.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        die_errno("mkstemp");
    size_t len = 16 << 20;
    char chunk[1 << 16];
    memset(chunk, 'x', sizeof chunk);
    for (size_t off = 0; off < len; off += sizeof chunk) /* real data, not a hole */
        if (write(fd, chunk, sizeof chunk) != (ssize_t)sizeof chunk)
            die_errno("write");

    int before = count_lines("/proc/self/maps");
    long rss_file0 = proc_status_field(0, "RssFile");
    long rss_shm0 = proc_status_field(0, "RssShmem");
    char *m = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED)
        die_errno("mmap");
    int after = count_lines("/proc/self/maps");
    print_region("mmap(file)", m);
    printf("  /proc/self/maps: %d lines before, %d after\n", before, after);

    volatile char sink = 0;
    for (size_t i = 0; i < len; i += (size_t)page_size)
        sink ^= m[i]; /* touch every page */
    (void)sink;
    printf("  before reading: RssFile %6ld kB  RssShmem %6ld kB\n", rss_file0, rss_shm0);
    printf("  after  reading: RssFile %6ld kB  RssShmem %6ld kB\n", proc_status_field(0, "RssFile"),
           proc_status_field(0, "RssShmem"));
    printf("mapped file pages come from the page cache and count as RssFile\n"
           "(or RssShmem when /tmp is a tmpfs), never as RssAnon.\n");
    munmap(m, len);
    close(fd);
    unlink(path);
}

static void exp_cow(size_t mb)
{
    section("4. fork() and copy-on-write");
    size_t bytes = mb << 20;
    char *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        die_errno("mmap");
    madvise(p, bytes, MADV_NOHUGEPAGE); /* one fault per 4 KiB page, for clarity */
    memset(p, 1, bytes);
    fflush(stdout);

    pid_t pid = fork();
    if (pid < 0)
        die_errno("fork");
    if (pid == 0) {
        long f0 = minor_faults();
        volatile char sink = 0;
        for (size_t i = 0; i < bytes; i += (size_t)page_size)
            sink ^= p[i]; /* read: pages are still shared */
        (void)sink;
        long f1 = minor_faults();
        uint64_t t0 = now_ns();
        for (size_t i = 0; i < bytes; i += (size_t)page_size)
            p[i] = 2; /* write: every page gets copied */
        uint64_t t1 = now_ns();
        long f2 = minor_faults();
        size_t pages = bytes / (size_t)page_size;
        printf("  child read  %zu pages: %6ld page faults\n", pages, f1 - f0);
        printf("  child wrote %zu pages: %6ld page faults, %.1f ms (%.2f us per copied page)\n",
               pages, f2 - f1, (double)(t1 - t0) / 1e6,
               (double)(t1 - t0) / 1e3 / (double)(f2 - f1 > 0 ? f2 - f1 : 1));
        fflush(stdout);
        _exit(0);
    }
    waitpid(pid, NULL, 0);
    printf("  parent still sees p[0] = %d\n", p[0]);
    printf("fork() copies only page tables; each page is duplicated lazily the first\n"
           "time either process writes to it. Reads never copy.\n");
    munmap(p, bytes);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    page_size = sysconf(_SC_PAGESIZE);
    size_t mb = argc > 1 ? (size_t)atol(argv[1]) : 256;
    if (mb < 2)
        mb = 2;
    exp_lazy(mb);
    exp_malloc_sources();
    exp_file_mapping();
    exp_cow(mb / 4 ? mb / 4 : 1);
    return 0;
}
