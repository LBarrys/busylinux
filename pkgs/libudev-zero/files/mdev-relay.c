/*
 * libudev-zero-mdev: runs BusyBox mdev for each kernel uevent, then
 * rebroadcasts it to netlink group 4 for libudev-zero, as mdevd -O 4 does.
 * Returns once listening; the daemon stays in the background.
 *
 * SPDX-License-Identifier: ISC
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/netlink.h>

#define MAX_ENV 64 /* UEVENT_NUM_ENVP */

static void mdev(char *arg, char **env)
{
    char *argv[] = { "mdev", arg, NULL };
    pid_t pid;

    if (posix_spawn(&pid, "/bin/busybox", NULL, NULL, argv, env) == 0)
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
}

int main(void)
{
    struct sockaddr_nl kernel = { .nl_family = AF_NETLINK, .nl_groups = 1 };
    struct sockaddr_nl udev = { .nl_family = AF_NETLINK, .nl_groups = 4 };
    static char *scan_env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL };
    char buf[8192], *env[MAX_ENV + 2], *p;
    int fd, fl, size = 32 << 20;

    fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        perror("libudev-zero-mdev: socket");
        return 1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &size, sizeof(size)) < 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    if (bind(fd, (struct sockaddr *)&kernel, sizeof(kernel)) < 0) {
        perror("libudev-zero-mdev: bind");
        return 1;
    }

    switch (fork()) {
    case -1:
        perror("libudev-zero-mdev: fork");
        return 1;
    case 0:
        break;
    default:
        return 0;
    }
    setsid();
    if (chdir("/") < 0)
        return 1;
    if ((fl = open("/dev/null", O_RDWR)) >= 0) {
        dup2(fl, 0);
        dup2(fl, 1);
        dup2(fl, 2);
        if (fl > 2)
            close(fl);
    }

    for (;;) {
        struct sockaddr_nl from;
        struct iovec iov = { buf, sizeof(buf) - 1 };
        struct msghdr msg = {
            .msg_name = &from, .msg_namelen = sizeof(from),
            .msg_iov = &iov, .msg_iovlen = 1,
        };
        ssize_t len = recvmsg(fd, &msg, 0);
        int n = 0;

        if (len < 0) {
            /* Events were lost: resync the nodes. */
            if (errno == ENOBUFS)
                mdev("-s", scan_env);
            continue;
        }
        if (msg.msg_flags & MSG_TRUNC || from.nl_pid != 0)
            continue; /* whole messages from the kernel only */
        buf[len] = '\0';

        /* The KEY=VALUE strings are mdev's environment. */
        env[n++] = scan_env[0];
        for (p = buf; p < buf + len && n <= MAX_ENV; p += strlen(p) + 1)
            if (strchr(p, '='))
                env[n++] = p;
        env[n] = NULL;
        mdev(NULL, env);

        sendto(fd, buf, len, 0, (struct sockaddr *)&udev, sizeof(udev));
    }
}
