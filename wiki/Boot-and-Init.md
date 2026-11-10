# Boot and init

## From power on to login

1. **The firmware** starts Limine from `\EFI\BOOT\BOOTX64.EFI` on the ESP.
2. **Limine** reads `limine.conf` (3-second menu) and loads `vmlinuz-busylinux`
   and `amd-ucode.img` (the CPU microcode, as an early module). There is no
   initramfs.
3. **The kernel** finds the partition named `busylinux-root` (`rootwait` waits
   for the NVMe to appear), mounts it read-only, and runs `/sbin/init`, BusyBox's.
4. **BusyBox init** reads `/etc/inittab` and runs `/etc/init.d/rcS` once.
5. Then it starts `busylinux-supervise` (runit's `runsvdir` for the services)
   and `getty` on tty1-3.

### The Limine entries

| Entry | |
|---|---|
| BusyLinux | the normal boot |
| BusyLinux (serial console) | the console also on `ttyS0` at 115200 baud (CI's boot test uses it) |
| Rescue shell | `init=/bin/sh`: a root shell, no services, root still read-only. `mount -o remount,rw /` to change files, `sync` and `reboot -f` when done |

## rcS, step by step

`/etc/init.d/rcS` (in `busylinux-init`):

1. **Virtual file systems**: `/proc`, `/sys`, `/dev` (devtmpfs), `/run`,
   `/dev/pts`, `/dev/shm`, efivarfs, and cgroup2 at `/sys/fs/cgroup` with every
   controller delegated. Also `/run/user` (1777) and the `/dev/fd` and
   `/dev/std*` links that udev would make and mdevd does not.
2. **Root check**: the kernel calls the root it mounted `/dev/root`, which is
   not a real node, so rcS finds the real one via `/proc/self/mountinfo` and
   `/sys/dev/block`. Then `fsck.ext4 -p` checks it. If it repaired something it
   reboots. If it cannot repair it alone, you get a shell to run `fsck.ext4` by
   hand; `exit` carries on.
3. **Remount** `/` read-write and `mount -a` (`/boot`, `/tmp`).
4. **Hostname**, console log level 3 (warnings stay off the console), and the
   **keymap** from `/etc/keymap/*.bmap.gz`.
5. **zram swap**: half the RAM, zstd, with `swappiness=180` and
   `page-cluster=0`. Swapping to compressed RAM is cheap, so the kernel may
   use it freely instead of dropping file cache.
6. **sysctls** from `/etc/sysctl.d/*.conf` and `/etc/sysctl.conf`.
7. **Devices**: `mdevd -O 4` with `/etc/mdev.conf`, then `mdevd-coldplug` for
   devices already present. rcS waits for mdevd to report ready. If mdevd fails,
   BusyBox `mdev -d` takes over. See [Desktop](Desktop.md#devices).
8. **Modules** listed in `/etc/modules-load.d/*.conf`.
9. **Device modes** the device manager does not set: `/dev/ntsync` 666 (Wine),
   `/dev/uinput` `input:660` (Steam Input).
10. **The firewall**: `nft -f /etc/nftables.conf`. If it fails, the network
    stays down; see [Network and firewall](Network-and-Firewall.md).
11. `lo` up.
12. **TRIM**: a minute later, in the background, `fstrim` on every ext4 mount.
13. **`/etc/rc.local`**, if executable: your own boot commands. apk keeps your
    version on upgrades.

## Services: runit

`busylinux-supervise` sets the mimalloc preload (below) and runs
`runsvdir /etc/service`. The runit tools (`runsvdir`, `runsv`, `sv`, `svlogd`,
`chpst`) are BusyBox's, from this repository's `busybox-runit`: Alpine's own
BusyBox leaves them out. They behave like runit's, and `sv NAME` means
`/etc/service/NAME`. A system installed before `busybox-runit` switches over
with `apk add busybox-runit@busylinux && apk del runit`; `doctor` warns while
both are installed. Each directory there is a service with a `run`
script, and optionally `log/run`:

| Service | |
|---|---|
| `syslogd` | BusyBox syslog, to `/var/log/messages` |
| `klogd` | kernel messages into syslog |
| `crond` | BusyBox cron, `/etc/crontabs/root` |
| `ntpd` | BusyBox NTP client |
| `acpid` | the power button powers off (`/etc/acpi/PWRF/00000080`) |
| `dhcp` | `udhcpc` on the first wired card; see [Network](Network-and-Firewall.md) |
| `seatd` | once `seatd` is installed: seat access for Wayland, group `seat` |
| `dbus` | once `dbus` is installed: the system bus |
| `scx` | the sched_ext scheduler `/etc/scx.conf` names; none by default (below) |

`seatd` and `dbus` sleep for a minute and look again while their program is
missing, so installing it later is enough. No reboot needed.

```sh
sv status /etc/service/*      # all of them
sv restart /etc/service/dhcp  # one
sv down /etc/service/ntpd     # stop until the next boot (touch .../down to keep it down)
```

Adding a service:

```sh
doas mkdir -p /etc/service/foo
printf '#!/bin/sh\nexec foo --foreground 2>&1\n' | doas tee /etc/service/foo/run
doas chmod 755 /etc/service/foo/run
```

runsvdir notices it within five seconds. The program must stay in the
foreground. [Docker](Docker-and-ROCm.md) is a full example with a log.

## A sched_ext scheduler

The kernel can hand CPU scheduling to a BPF scheduler (see
[Hardware and kernel](Hardware-and-Kernel.md#sched_ext)). The `scx` service
runs the one `/etc/scx.conf` names:

```sh
# /etc/scx.conf
SCHEDULER=scx_simple
ARGS=
```

Then `doas sv restart /etc/service/scx`, or wait a minute: with no scheduler
named, the service checks the file again every minute. To go back to the
kernel's own scheduler, empty `SCHEDULER=` and restart the service.

The scheduler's per-second statistics are thrown away; its errors go to
syslog (`grep scx /var/log/messages`). If it exits or the kernel's watchdog
stops it, the kernel switches back to EEVDF at once, and runit starts the
scheduler again a second later. `doctor` warns when a scheduler is named but
none runs.

Is it worth it? For gaming on a single-CCD CPU, EEVDF is already good;
`scx_simple` can smooth frame times when the CPU is fully loaded (shader
compilation, background builds), and costs nothing to try. Measure with
MangoHud's frame-time graph rather than average FPS.

## Logins

`/etc/inittab` starts `getty` on tty1-3 under three limits:

- `ulimit -e 40 -r 95`: may use nice -11 and realtime priority up to 95, which PipeWire asks for. No rtkit needed.
- `ulimit -Hn 524288`: up to 524288 open files, which Proton's esync wants. The soft limit stays 1024 until a program raises it.

`/etc/profile.d/busylinux.sh` sets `XDG_RUNTIME_DIR=/run/user/UID` (made and
owned by you, mode 0700), the mimalloc preload, and shows the update notice.

`doas` lets `wheel` run anything as root, remembering your password for a
while (`permit persist :wheel`).

## Memory: mimalloc

musl's allocator is small and slow under threads. Services (through
`busylinux-supervise`) and logins (through `profile.d`) preload
`libmimalloc.so.3`, mimalloc in **secure mode**: guard pages, encrypted free
lists and randomised allocation. musl has no `/etc/ld.so.preload`, so this is
done with `LD_PRELOAD`. setuid programs such as `doas` ignore it and keep
musl's allocator, as they should.

The cost: programs use more memory than with musl's allocator; Firefox shows
it most, as mimalloc keeps freed memory around per thread for reuse. To run one program without it:
`env -u LD_PRELOAD firefox`.

Programs in the Arch tree never get it: glibc cannot load a musl library, so
`arch-chroot` drops `LD_PRELOAD`.

## Shutdown

`poweroff`, `reboot`, the power button, or Ctrl+Alt+Del (reboot). `rcK`:

1. gives every runit service 20 seconds to stop (`sv -w 20 force-stop`)
2. sends everything else SIGTERM and waits up to 5 seconds, then SIGKILL
3. saves the clock to the RTC, syncs, remounts `/` read-only and unmounts the rest

## sysctls

`/etc/sysctl.d/10-hardening.conf`:

| | |
|---|---|
| `kernel.kptr_restrict=2` | no kernel addresses in `/proc` |
| `kernel.unprivileged_bpf_disabled=1`, `net.core.bpf_jit_harden=2` | BPF for root only, JIT hardened |
| `kernel.io_uring_disabled=2` | io_uring off: much of the kernel's recent exploits, and nothing here needs it |
| `kernel.sysrq=244` | SysRq for REISUB only |
| `fs.protected_*` | the usual link, FIFO and regular-file protections in sticky directories |
| `rp_filter`, no redirects or source routes | the usual network hardening |

`/etc/sysctl.d/20-games.conf` sets `vm.max_map_count=1048576`, as Arch,
Fedora and Ubuntu do; some games under Proton crash with the default 65530.
