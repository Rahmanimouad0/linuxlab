/*
 * socket_lab - Unix domain sockets: bidirectional, connection-oriented IPC
 * between processes on the same machine.
 *
 *   client ──connect()──▶ /tmp/linuxlab-<pid>.sock ◀──accept()── server
 *
 *   1. an echo server and a client (socket/bind/listen/accept/connect),
 *      with SO_PEERCRED: the kernel tells the server who the client is
 *   2. SCM_RIGHTS: passing an open file descriptor to another process
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

static void run_server(int listen_fd)
{
    int c = accept(listen_fd, NULL, NULL);
    if (c < 0)
        die_errno("accept");

    struct ucred cred;
    socklen_t len = sizeof cred;
    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0)
        printf("  server: client is pid %d, uid %d, gid %d (from SO_PEERCRED)\n", (int)cred.pid,
               (int)cred.uid, (int)cred.gid);

    char buf[256];
    ssize_t n;
    while ((n = read(c, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n; i++)
            buf[i] = (char)toupper((unsigned char)buf[i]);
        if (write(c, buf, (size_t)n) != n)
            break;
    }
    close(c);
}

static void exp_echo(void)
{
    section("1. echo server over a Unix socket");
    char path[108];
    snprintf(path, sizeof path, "/tmp/linuxlab-%d.sock", (int)getpid());
    unlink(path);

    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0)
        die_errno("socket");
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0)
        die_errno("bind");
    if (listen(lfd, 4) < 0)
        die_errno("listen");
    printf("  server listening on %s\n", path);

    pid_t server = fork();
    if (server == 0) {
        run_server(lfd);
        _exit(0);
    }
    close(lfd); /* only the server needs the listening socket */

    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (connect(s, (struct sockaddr *)&addr, sizeof addr) < 0)
        die_errno("connect");
    const char *msgs[] = {"hello", "unix domain sockets"};
    for (size_t i = 0; i < ARRAY_LEN(msgs); i++) {
        char buf[64] = {0};
        if (write(s, msgs[i], strlen(msgs[i])) < 0 || read(s, buf, sizeof buf - 1) < 0)
            die_errno("echo");
        printf("  client: sent \"%s\", got \"%s\"\n", msgs[i], buf);
    }
    close(s); /* server's read() returns 0 -> it exits */
    waitpid(server, NULL, 0);
    unlink(path);
}

static int send_fd(int sock, int fd)
{
    char dummy = 'F';
    struct iovec iov = {&dummy, 1};
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } u;
    memset(&u, 0, sizeof u);
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof u.buf;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    return sendmsg(sock, &msg, 0) < 0 ? -1 : 0;
}

static int recv_fd(int sock)
{
    char dummy;
    struct iovec iov = {&dummy, 1};
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } u;
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof u.buf;
    if (recvmsg(sock, &msg, 0) <= 0)
        return -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    if (!c || c->cmsg_type != SCM_RIGHTS)
        return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(c), sizeof fd);
    return fd;
}

static void exp_pass_fd(void)
{
    section("2. passing a file descriptor (SCM_RIGHTS)");
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        die_errno("socketpair");

    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        /* Burn a few descriptor numbers so the received fd gets a new one. */
        for (int i = 0; i < 3; i++)
            if (dup(STDERR_FILENO) < 0)
                _exit(1);
        int fd = recv_fd(sv[1]);
        char buf[128] = {0};
        ssize_t n = read(fd, buf, sizeof buf - 1);
        if (n > 0)
            buf[strcspn(buf, "\n")] = '\0';
        printf("  child : received fd %d and read the first line: \"%s\"\n", fd, buf);
        _exit(0);
    }
    close(sv[1]);

    /* The child could not open this file itself if it lacked permission:
     * passing the descriptor passes the *access*, not the path. */
    int fd = open("/proc/version", O_RDONLY);
    if (fd < 0)
        die_errno("open");
    printf("  parent: opened /proc/version as fd %d, sending it\n", fd);
    if (send_fd(sv[0], fd) < 0)
        die_errno("sendmsg");
    close(fd);
    waitpid(pid, NULL, 0);
    close(sv[0]);
    printf("the number changes, the open file description is the same. This is\n"
           "how privilege-separated servers hand sockets/files to workers.\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    exp_echo();
    exp_pass_fd();
    return 0;
}
