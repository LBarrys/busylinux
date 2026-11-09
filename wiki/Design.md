# Design

BusyLinux aims to be **small, minimal and simple**: a system one person can
read end to end, for one machine, that still runs a modern desktop and
Windows games. This page explains the main choices.

## Alpine, not a distribution of its own

Building every package would be a full-time job. Alpine already is small,
musl-based, security-minded and fast-moving on edge. BusyLinux takes it as it
is, and builds only what has to differ: nine recipes. Everything else updates
with `apk upgrade`, and Alpine's wiki applies.

**Edge, not a stable branch**, because the desktop and Mesa move quickly and
games want new drivers. The cost is the occasional breakage. A rolling release
on one machine with a boot-tested image is a fair trade.

## musl

musl is smaller and simpler than glibc, and its code is far easier to read.
Its weak points are a slow allocator under threads, fixed here with mimalloc,
and programs that assume glibc. The ones that matter here are Steam and
Proton, and they get a glibc tree of their own.

## BusyBox init and runit, not OpenRC or systemd

The whole boot is `rcS`, about 110 lines of `sh` that run top to bottom.
There is no dependency solver, no unit files, and no state between boots.
runit supervises the long-running services: each is a folder with a `run`
script that execs the program in the foreground, and runit restarts it if it
dies. Ordering needs no graph: a service that needs something else waits for
it in its own `run` script, as `dhcp` does for syslog and the firewall.

## No initramfs

An initramfs exists to find and mount the root when the kernel cannot do it
alone: modules for the disk, LVM, LUKS, network roots. Here the disk drivers
are built in, and the root is a plain ext4 partition the kernel finds by
partition name. So there is nothing to put in an initramfs. Without one, the
boot has one less image to rebuild, sign and go stale. The cost: no root
encryption, which would need an initramfs (or a kernel-built dm-crypt setup)
to ask for the passphrase.

## tinyconfig

A distribution kernel builds thousands of drivers as modules for hardware
it might meet. This one starts from the smallest configuration and adds what
this machine and its workloads need, so there is less code to load, attack
or break. The guard lists in the recipe keep it that way across kernel
updates: a needed option that disappears, or an excluded one that returns,
fails the build.

## mdevd and libudev-zero, not udev

udev is part of systemd, and its fork eudev trails behind. mdevd is a small netlink
device manager that reads mdev's simple configuration, and libudev-zero gives
programs the libudev API by reading `/sys` directly. What is lost: udev's
rules database and properties, such as persistent names, `ID_INPUT_*`
tagging and `uaccess`. Most of it is not needed here; the rest is a
line in `mdev.conf` or a mode set in `rcS`.

## mimalloc everywhere it can go

Secure-mode mimalloc is both faster than musl's allocator and harder to
exploit: guard pages, randomised allocation, encrypted free lists. musl has no
`ld.so.preload`, so it is preloaded for services and logins with `LD_PRELOAD`.
That is not a system-wide guarantee: a program started without that environment
uses musl's allocator. setuid programs always do, by design. The cost is
memory, see [Boot and init](Boot-and-Init.md#memory-mimalloc).

## Failing closed

Where a mistake could leave the machine open, it is made to fail shut
instead. A firewall that does not load keeps the network down. Modules
must be signed. io_uring is off. A kernel build that loses a hardening
option does not finish.

## Steam in a tree, not on the system

Steam needs glibc and 32-bit libraries. Bringing them onto a musl system
would mean two C libraries side by side everywhere. A separate tree keeps
them in one folder, read-only to the programs that use it, updated
apart, and removable with `rm -rf`. bubblewrap gives each program the view
it needs without a daemon.

## Signing and pinning

Every package from this repository is signed: by the image's key from the
build container, and by a machine-local key from `update.sh`. It is pinned to
the local repository with `@busylinux`. Alpine's mirror can therefore never
replace the kernel or init with its own builds, and a tampered local
package would not install.

## What was left out on purpose

| Left out | Why |
|---|---|
| Wi-Fi, Bluetooth | the machine is wired; less code in the kernel |
| HDA audio | the board's audio is on USB |
| KVM | not used; containers cover the rest |
| Secure Boot | Limine and the kernel would need signing with enrolled keys; full-disk encryption would be needed for it to mean much |
| AppArmor, SELinux | a policy for every program is a project of its own; Landlock and bubblewrap sandbox what needs it |
| Polkit, logind, elogind | seatd gives seat access, and `doas` the rest |
| Flatpak | see [Steam and games](Steam-and-Games.md#why-not) |
| sched_ext schedulers | they need BTF debug info in the kernel and clang to build; EEVDF serves an 8-core single-CCD CPU well |
| An initramfs | see above |
