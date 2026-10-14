/*
 * bench_fileio - read() vs mmap() for scanning a large file.
 *
 *   read 4KiB / 64KiB / 1MiB   copy the file into a user buffer, chunk by chunk
 *   mmap                        map the file, touch the pages in place
 *   mmap+MAP_POPULATE           map and pre-fault all pages in one syscall
 *
 * Every variant sums the file as 64-bit words, so the CPU work is the same;
 * the difference is how the bytes get from the page cache to the program:
 * read() copies them (one syscall per chunk), mmap() maps the page-cache
 * pages directly (one page fault per page, or per fault-around batch).
 *
 * The file is read once before measuring, so all runs are "warm cache":
 * this compares the two *interfaces*, not disk speed.
 *
 * The file is created in $LINUXLAB_BENCH_DIR (default: current directory).
 * Note: if that directory is on tmpfs, the "file" already lives in RAM.
 */
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bench.h"
#include "util.h"

static uint64_t sum_words(const uint64_t *p, size_t n)
{
    uint64_t s = 0;
    for (size_t i = 0; i < n; i++)
        s += p[i];
    return s;
}

static uint64_t scan_read(const char *path, size_t chunk)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        die_errno("open");
    uint64_t *buf = aligned_alloc(64, chunk);
    if (!buf)
        die("out of memory");
    uint64_t s = 0;
    ssize_t n;
    while ((n = read(fd, buf, chunk)) > 0)
        s += sum_words(buf, (size_t)n / sizeof *buf);
    free(buf);
    close(fd);
    return s;
}

static uint64_t scan_mmap(const char *path, size_t size, int populate)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        die_errno("open");
    const uint64_t *p =
        mmap(NULL, size, PROT_READ, MAP_PRIVATE | (populate ? MAP_POPULATE : 0), fd, 0);
    if (p == MAP_FAILED)
        die_errno("mmap");
    madvise((void *)p, size, MADV_SEQUENTIAL);
    uint64_t s = sum_words(p, size / sizeof *p);
    munmap((void *)p, size);
    close(fd);
    return s;
}

int main(int argc, char **argv)
{
    bench_parse_args(argc, argv);
    const int reps = bench_quick ? 3 : 7;
    const size_t size = bench_quick ? (16u << 20) : (512u << 20);
    const char *dir = getenv("LINUXLAB_BENCH_DIR");
    char path[4096];
    snprintf(path, sizeof path, "%s/linuxlab-bench-%d.dat", dir ? dir : ".", (int)getpid());

    /* Create the test file with a non-trivial pattern. */
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0)
        die_errno(path);
    uint64_t *chunk = xmalloc(1 << 20);
    for (size_t off = 0, k = 0; off < size; off += 1 << 20) {
        for (size_t i = 0; i < (1 << 20) / sizeof *chunk; i++)
            chunk[i] = k++;
        if (write(fd, chunk, 1 << 20) != 1 << 20)
            die_errno("write");
    }
    free(chunk);
    fsync(fd);
    close(fd);

    uint64_t expected = scan_read(path, 1 << 20); /* also warms the page cache */
    char param[32];
    snprintf(param, sizeof param, "file=%zuMiB", size >> 20);

    bench_header("sequential scan of a cached file (higher is better)");
    struct {
        const char *name;
        size_t chunk;
        int mode; /* 0 read, 1 mmap, 2 mmap+populate */
    } variants[] = {
        {"read 4KiB", 4096, 0}, {"read 64KiB", 65536, 0},    {"read 1MiB", 1 << 20, 0},
        {"mmap", 0, 1},         {"mmap+MAP_POPULATE", 0, 2},
    };
    for (size_t v = 0; v < ARRAY_LEN(variants); v++) {
        double t[16] = {0};
        for (int r = 0; r < reps; r++) {
            uint64_t t0 = now_ns();
            uint64_t s = variants[v].mode == 0 ? scan_read(path, variants[v].chunk)
                                               : scan_mmap(path, size, variants[v].mode == 2);
            uint64_t t1 = now_ns();
            if (s != expected)
                die("checksum mismatch in %s", variants[v].name);
            t[r] = (double)size / (1 << 20) / ((double)(t1 - t0) / 1e9);
        }
        report("fileio", variants[v].name, param, median(t, reps), "MiB/s");
    }
    unlink(path);
    return 0;
}
