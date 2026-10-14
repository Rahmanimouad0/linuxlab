/*
 * llfs - filesystem metadata and permissions, straight from the syscalls.
 *
 *   llfs tree [-a] [-L depth] [path]   recursive listing (scandir + lstat)
 *   llfs stat PATH...                  everything struct stat knows
 *   llfs perms PATH...                 explain rwx bits and what *you* may do
 *   llfs umask                         the umask and the modes it produces
 */
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

/* ------------------------------------------------------------ helpers */

static const char *type_name(mode_t m)
{
    if (S_ISREG(m))
        return "regular file";
    if (S_ISDIR(m))
        return "directory";
    if (S_ISLNK(m))
        return "symbolic link";
    if (S_ISFIFO(m))
        return "FIFO (named pipe)";
    if (S_ISSOCK(m))
        return "socket";
    if (S_ISCHR(m))
        return "character device";
    if (S_ISBLK(m))
        return "block device";
    return "unknown";
}

static char type_char(mode_t m)
{
    if (S_ISDIR(m))
        return 'd';
    if (S_ISLNK(m))
        return 'l';
    if (S_ISFIFO(m))
        return 'p';
    if (S_ISSOCK(m))
        return 's';
    if (S_ISCHR(m))
        return 'c';
    if (S_ISBLK(m))
        return 'b';
    return '-';
}

/* "rwx" for one class; special bits replace the x like ls does. */
static void class_bits(mode_t m, int shift, mode_t special, char spec_ch, char out[4])
{
    out[0] = (m >> shift) & 4 ? 'r' : '-';
    out[1] = (m >> shift) & 2 ? 'w' : '-';
    int x = (m >> shift) & 1;
    if (m & special)
        out[2] = x ? spec_ch : (char)(spec_ch - 32); /* s/S, t/T */
    else
        out[2] = x ? 'x' : '-';
    out[3] = '\0';
}

static void mode_string(mode_t m, char out[11])
{
    char u[4], g[4], o[4];
    class_bits(m, 6, S_ISUID, 's', u);
    class_bits(m, 3, S_ISGID, 's', g);
    class_bits(m, 0, S_ISVTX, 't', o);
    snprintf(out, 11, "%c%s%s%s", type_char(m), u, g, o);
}

static const char *user_of(uid_t uid)
{
    struct passwd *pw = getpwuid(uid);
    return pw ? pw->pw_name : "?";
}

static const char *group_of(gid_t gid)
{
    struct group *gr = getgrgid(gid);
    return gr ? gr->gr_name : "?";
}

static void print_time(const char *label, const struct timespec *ts)
{
    char buf[64];
    struct tm tm;
    localtime_r(&ts->tv_sec, &tm);
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    printf("%8s: %s.%09ld\n", label, buf, ts->tv_nsec);
}

/* -------------------------------------------------------------- tree */

typedef struct {
    int all;
    int max_depth;
    long dirs, files;
} tree_opts;

static int not_hidden(const struct dirent *e) { return e->d_name[0] != '.'; }
static int not_dots(const struct dirent *e)
{
    return strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0;
}

static void tree(const char *path, const char *prefix, int depth, tree_opts *o)
{
    struct dirent **list;
    int n = scandir(path, &list, o->all ? not_dots : not_hidden, alphasort);
    if (n < 0) {
        printf("%s[error opening dir: %s]\n", prefix, strerror(errno));
        return;
    }
    for (int i = 0; i < n; i++) {
        const char *name = list[i]->d_name;
        int last = i == n - 1;
        char child[PATH_MAX];
        snprintf(child, sizeof child, "%s/%s", path, name);

        struct stat st;
        /* lstat: never follow symlinks, so a link to "/" cannot loop */
        if (lstat(child, &st) < 0) {
            printf("%s%s%s [%s]\n", prefix, last ? "└── " : "├── ", name, strerror(errno));
            free(list[i]);
            continue;
        }
        printf("%s%s%s", prefix, last ? "└── " : "├── ", name);
        if (S_ISDIR(st.st_mode)) {
            putchar('/');
            o->dirs++;
        } else {
            o->files++;
        }
        if (S_ISLNK(st.st_mode)) {
            char target[PATH_MAX];
            ssize_t len = readlink(child, target, sizeof target - 1);
            if (len >= 0) {
                target[len] = '\0';
                printf(" -> %s", target);
            }
        }
        putchar('\n');

        if (S_ISDIR(st.st_mode) && (o->max_depth < 0 || depth + 1 < o->max_depth)) {
            char next[PATH_MAX];
            snprintf(next, sizeof next, "%s%s", prefix, last ? "    " : "│   ");
            tree(child, next, depth + 1, o);
        }
        free(list[i]);
    }
    free(list);
}

static int cmd_tree(int argc, char **argv)
{
    tree_opts o = {0, -1, 0, 0};
    const char *path = ".";
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0)
            o.all = 1;
        else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc)
            o.max_depth = atoi(argv[++i]);
        else
            path = argv[i];
    }
    struct stat st;
    if (stat(path, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "llfs: %s: not a directory\n", path);
        return 1;
    }
    printf("%s\n", path);
    tree(path, "", 0, &o);
    printf("\n%ld directories, %ld files\n", o.dirs, o.files);
    return 0;
}

/* -------------------------------------------------------------- stat */

static int cmd_stat(int argc, char **argv)
{
    int rc = 0;
    for (int i = 0; i < argc; i++) {
        struct stat st;
        if (lstat(argv[i], &st) < 0) {
            fprintf(stderr, "llfs: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        char mode[11];
        mode_string(st.st_mode, mode);
        printf("    File: %s", argv[i]);
        if (S_ISLNK(st.st_mode)) {
            char target[PATH_MAX];
            ssize_t n = readlink(argv[i], target, sizeof target - 1);
            if (n >= 0) {
                target[n] = '\0';
                printf(" -> %s", target);
            }
        }
        printf("\n    Type: %s\n", type_name(st.st_mode));
        printf("    Size: %lld bytes   Blocks: %lld (512-byte units)   IO block: %ld\n",
               (long long)st.st_size, (long long)st.st_blocks, (long)st.st_blksize);
        printf("   Inode: %llu   Links: %lu   Device: %u,%u\n", (unsigned long long)st.st_ino,
               (unsigned long)st.st_nlink, major(st.st_dev), minor(st.st_dev));
        printf("    Mode: %04o (%s)\n", (unsigned)(st.st_mode & 07777), mode);
        printf("   Owner: %s (%u)   Group: %s (%u)\n", user_of(st.st_uid), (unsigned)st.st_uid,
               group_of(st.st_gid), (unsigned)st.st_gid);
        print_time("Access", &st.st_atim);
        print_time("Modify", &st.st_mtim);
        print_time("Change", &st.st_ctim);
        if (st.st_blocks * 512 < st.st_size)
            printf("    Note: fewer blocks than bytes -> sparse file (holes use no disk)\n");
        if (i + 1 < argc)
            putchar('\n');
    }
    return rc;
}

/* ------------------------------------------------------------- perms */

static int in_group(gid_t gid)
{
    if (getegid() == gid)
        return 1;
    gid_t groups[256];
    int n = getgroups((int)ARRAY_LEN(groups), groups);
    for (int i = 0; i < n; i++)
        if (groups[i] == gid)
            return 1;
    return 0;
}

static void describe_bits(const char *bits, int is_dir, char *out, size_t len)
{
    const char *r = is_dir ? "list names" : "read";
    const char *w = is_dir ? "create/delete entries" : "write";
    const char *x = is_dir ? "enter/traverse" : "execute";
    out[0] = '\0';
    if (bits[0] == 'r')
        snprintf(out + strlen(out), len - strlen(out), "%s, ", r);
    if (bits[1] == 'w')
        snprintf(out + strlen(out), len - strlen(out), "%s, ", w);
    if (bits[2] != '-' && bits[2] != 'S' && bits[2] != 'T')
        snprintf(out + strlen(out), len - strlen(out), "%s, ", x);
    size_t l = strlen(out);
    if (l >= 2)
        out[l - 2] = '\0';
    else
        snprintf(out, len, "nothing");
}

static int cmd_perms(int argc, char **argv)
{
    int rc = 0;
    for (int i = 0; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            fprintf(stderr, "llfs: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        int is_dir = S_ISDIR(st.st_mode);
        char mode[11], u[4], g[4], o[4], desc[128];
        mode_string(st.st_mode, mode);
        class_bits(st.st_mode, 6, S_ISUID, 's', u);
        class_bits(st.st_mode, 3, S_ISGID, 's', g);
        class_bits(st.st_mode, 0, S_ISVTX, 't', o);

        printf("file:    %s (%s)\n", argv[i], type_name(st.st_mode));
        printf("mode:    %s  (%04o)\n", mode, (unsigned)(st.st_mode & 07777));
        describe_bits(u, is_dir, desc, sizeof desc);
        printf("owner:   %s  %-12s %s\n", u, user_of(st.st_uid), desc);
        describe_bits(g, is_dir, desc, sizeof desc);
        printf("group:   %s  %-12s %s\n", g, group_of(st.st_gid), desc);
        describe_bits(o, is_dir, desc, sizeof desc);
        printf("other:   %s  %-12s %s\n", o, "(everyone)", desc);

        if (st.st_mode & (S_ISUID | S_ISGID | S_ISVTX)) {
            printf("special:");
            if (st.st_mode & S_ISUID)
                printf(" setuid (runs with the owner's uid)");
            if (st.st_mode & S_ISGID)
                printf(is_dir ? " setgid (new files inherit the group)"
                              : " setgid (runs with the file's group)");
            if (st.st_mode & S_ISVTX)
                printf(" sticky (only owners may delete entries)");
            putchar('\n');
        }

        /* The kernel picks exactly ONE class, in this order. */
        const char *cls, *bits;
        uid_t me = geteuid();
        if (me == 0) {
            cls = "root: permission bits are mostly bypassed";
            bits = "rw?";
        } else if (me == st.st_uid) {
            cls = "you own it -> only the OWNER bits apply";
            bits = u;
        } else if (in_group(st.st_gid)) {
            cls = "you are in its group -> only the GROUP bits apply";
            bits = g;
        } else {
            cls = "neither owner nor group -> the OTHER bits apply";
            bits = o;
        }
        printf("you:     uid %u: %s (%s)\n", (unsigned)me, cls, bits);
        printf("access(): read %s, write %s, execute %s\n",
               access(argv[i], R_OK) == 0 ? "yes" : "no", access(argv[i], W_OK) == 0 ? "yes" : "no",
               access(argv[i], X_OK) == 0 ? "yes" : "no");
        if (i + 1 < argc)
            putchar('\n');
    }
    return rc;
}

static int cmd_umask(void)
{
    mode_t m = umask(0);
    umask(m); /* umask() can only be read by setting it */
    char f[11], d[11];
    mode_string(S_IFREG | (0666 & ~m), f);
    mode_string(S_IFDIR | (0777 & ~m), d);
    printf("umask:              %04o\n", (unsigned)m);
    printf("new file  (0666 & ~umask): %04o %s\n", (unsigned)(0666 & ~m), f);
    printf("new dir   (0777 & ~umask): %04o %s\n", (unsigned)(0777 & ~m), d);
    printf("the umask removes bits; it can never add them. It is inherited across\n"
           "fork() and exec(), which is why it is set once in the login shell.\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "tree") == 0)
        return cmd_tree(argc - 2, argv + 2);
    if (argc >= 3 && strcmp(argv[1], "stat") == 0)
        return cmd_stat(argc - 2, argv + 2);
    if (argc >= 3 && strcmp(argv[1], "perms") == 0)
        return cmd_perms(argc - 2, argv + 2);
    if (argc == 2 && strcmp(argv[1], "umask") == 0)
        return cmd_umask();
    fprintf(stderr, "usage: llfs tree [-a] [-L depth] [path]\n"
                    "       llfs stat PATH...\n"
                    "       llfs perms PATH...\n"
                    "       llfs umask\n");
    return 2;
}
