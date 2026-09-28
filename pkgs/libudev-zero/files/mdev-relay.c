/*
 * libudev-zero-mdev: BusyBox mdev as the device manager for libudev-zero.
 *
 * Listens for kernel uevents, runs BusyBox mdev on each one (device nodes,
 * their modes and the modprobe rule of /etc/mdev.conf) and only then passes
 * the event on to netlink group 4, where libudev-zero's monitors listen.
 * That is what mdevd -O 4 does and mdev -d cannot. Events are handled one at
 * a time, in order, so a program told about a device finds its node in place
 * with its final mode.
 *
 * Returns once it listens, leaving the daemon in the background, so nothing
 * the caller does afterwards (mdev -s, loading modules) goes unseen.
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

#define MAX_ENV 64 /* the kernel's UEVENT_NUM_ENVP */

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
    /* Room for the burst of events a GPU driver makes as it loads. */
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
            /* Events were lost: bring the nodes up to date at least. */
            if (errno == ENOBUFS)
                mdev("-s", scan_env);
            continue;
        }
        if (msg.msg_flags & MSG_TRUNC || from.nl_pid != 0)
            continue; /* only whole messages, and only from the kernel */
        buf[len] = '\0';

        /* "ACTION@DEVPATH", then KEY=VALUE strings: those are mdev's
         * environment, as when the kernel runs it as its hotplug helper. */
        env[n++] = scan_env[0];
        for (p = buf; p < buf + len && n <= MAX_ENV; p += strlen(p) + 1)
            if (strchr(p, '='))
                env[n++] = p;
        env[n] = NULL;
        mdev(NULL, env);

        sendto(fd, buf, len, 0, (struct sockaddr *)&udev, sizeof(udev));
    }
}
