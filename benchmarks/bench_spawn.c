/*
 * bench_spawn - how expensive is creating a process?
 *
 * Variants (each creates a child and waits for it):
 *   fork+_exit       fork only, the child exits immediately
 *   fork+exec        fork, then exec /bin/true   (what lsh does)
 *   vfork+exec       vfork: child borrows the parent's memory until exec
 *   posix_spawn      glibc implements it with clone(CLONE_VM|CLONE_VFORK)
 *
 * Each is measured with a parent that has 0, 64 and 256 MB of *touched*
 * memory. fork() must copy the page tables of all of it; vfork and
 * posix_spawn do not, so their cost should stay flat.
 */
#include <spawn.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bench.h"
#include "util.h"

extern char **environ;

static char *const true_argv[] = {"true", NULL};
static const char *true_path = "/bin/true";

typedef enum { FORK_EXIT, FORK_EXEC, VFORK_EXEC, POSIX_SPAWN } variant;
static const char *variant_names[] = {"fork+_exit", "fork+exec", "vfork+exec", "posix_spawn"};

static void spawn_once(variant v)
{
    pid_t pid = -1;
    switch (v) {
    case FORK_EXIT:
        pid = fork();
        if (pid == 0)
            _exit(0);
        break;
    case FORK_EXEC:
        pid = fork();
        if (pid == 0) {
            execv(true_path, true_argv);
            _exit(127);
        }
        break;
    case VFORK_EXEC:
        pid = vfork();
        if (pid == 0) {
            execv(true_path, true_argv);
            _exit(127);
        }
        break;
    case POSIX_SPAWN:
        if (posix_spawn(&pid, true_path, NULL, NULL, true_argv, environ) != 0)
            pid = -1;
        break;
    }
    if (pid < 0)
        die_errno("spawn");
    int status;
    waitpid(pid, &status, 0);
}

int main(int argc, char **argv)
{
    bench_parse_args(argc, argv);
    const int reps = bench_quick ? 3 : 7;
    const int per_rep = bench_quick ? 20 : 200;
    const size_t sizes_mb[] = {0, 64, 256};
    const int nsizes = bench_quick ? 2 : 3;

    bench_header("process creation (microseconds per create+wait)");
    for (int s = 0; s < nsizes; s++) {
        size_t mb = bench_quick && s == 1 ? 16 : sizes_mb[s];
        char *mem = NULL;
        if (mb) {
            mem = mmap(NULL, mb << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mem == MAP_FAILED)
                die_errno("mmap");
            memset(mem, 1, mb << 20); /* touch it: page tables must exist */
        }
        char param[32];
        snprintf(param, sizeof param, "parent=%zuMB", mb);

        for (variant v = FORK_EXIT; v <= POSIX_SPAWN; v++) {
            double t[16] = {0};
            spawn_once(v); /* warm-up */
            for (int r = 0; r < reps; r++) {
                uint64_t t0 = now_ns();
                for (int i = 0; i < per_rep; i++)
                    spawn_once(v);
                t[r] = (double)(now_ns() - t0) / 1e3 / per_rep;
            }
            report("spawn", variant_names[v], param, median(t, reps), "us");
        }
        if (mem)
            munmap(mem, mb << 20);
    }
    return 0;
}
