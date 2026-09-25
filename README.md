# BusyLinux

A musl Linux distribution built on **Alpine Linux edge**, with a kernel
configured from `tinyconfig` for one specific machine and BusyBox init instead
of OpenRC.

Alpine owns the core. musl, BusyBox, apk-tools and every library come straight
from Alpine's repositories, so the ~36,000 packages in edge install and run
unmodified.

## The machine this kernel is for

```
CPU     AMD Ryzen 7 7800X3D (Zen 4, AM5, 8C/16T)
GPU     Radeon RX 7900 GRE (Navi 31) + Raphael iGPU  -> amdgpu
Board   MSI MAG B650 TOMAHAWK WIFI
          LAN     Realtek RTL8125BG 2.5G             -> r8169
          Audio   Realtek ALC4080                    -> USB Audio Class, not HDA
          Storage 3x NVMe, 6x SATA
RAM     32 GB DDR5, EXPO (firmware-side; the kernel needs nothing for it)
```

Wireless and Bluetooth are deliberately absent.
To build for different hardware, edit
`pkgs/linux-busylinux/files/busylinux.config`.

## Building

The build runs entirely inside an Arch Linux container; nothing is compiled on
the host.

```sh
apk add docker qemu-system-x86_64
rc-service docker start
addgroup "$USER" docker          # log out and back in

git clone https://github.com/LBarrys/busylinux.git
cd busylinux
docker build -t busylinux-builder .
mkdir -p out
docker run --rm -it \
  -v "$PWD/out:/build/out" -v busylinux-cache:/build/cache \
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
  busylinux-builder
```

| Variable | Effect |
|---|---|
| `PACKAGES="..."` | extra Alpine packages to bake into the image |
| `BASE_PACKAGES="..."` | replace the default package set |
| `ALPINE_BRANCH=v3.24` | build against a stable release instead of edge |
| `ALPINE_MIRROR=...` | a different mirror; the default is `https://mirror.maeen.sa/alpine` |
| `REPO_URL=https://...` | an extra network URL for this repository |
| `LOCAL_REPO=0` | do not copy this repository into the image |
| `VM_SUPPORT=0` | drop virtio and bochs: a kernel that only knows your hardware |
| `MENUCONFIG=1` | open `menuconfig` after the fragments are merged |
| `REBUILD=1` | rebuild packages even when a cached `.apk` exists |
| `JOBS=N` | parallel make jobs, default `nproc` |
| `IMAGE_SIZE=16G` | size of the sparse `disk.img` |

## Running under QEMU

```sh
qemu-system-x86_64 -m 2G -nographic \
  -kernel out/vmlinuz -initrd out/initramfs.cpio.gz \
  -append "root=LABEL=BUSYLINUX_ROOT console=ttyS0" \
  -drive file=out/disk.img,format=raw,if=virtio \
  -nic user,model=virtio-net-pci
```

Log in as `root` with no password.

## Installing on real hardware

`install.sh` does the whole thing. Run it as root from an Alpine live USB — any
Linux with the tools below will do, but Alpine is musl like the target, so the
chroot at the end works without surprises.

```sh
apk add sgdisk dosfstools e2fsprogs parted efibootmgr

./install.sh --disk /dev/nvme0n1 --user alice
```

The installer asks you to type the disk name before it touches anything, then
writes:

| Partition | Size | |
|---|---|---|
| 1 | 1 GiB | EFI system, FAT32, **mounted at `/boot`** |
| 2 | rest | root, ext4, `LABEL=BUSYLINUX_ROOT` |

| Option | |
|---|---|
| `--user NAME` | create a user in `video`, `input`, `audio`, `seat`, `wheel`, set a password |
| `--esp-size 2G` | bigger ESP, for keeping several kernels |
| `--root-size 200G` | leave the rest of the disk unpartitioned |
| `--hostname NAME` | default `busylinux` |
| `--no-nvram` | removable path only, do not touch the firmware boot menu |
| `--yes` | skip the confirmation |

## What rcS starts

`/etc/init.d/rcS` is deliberately conditional — the same script serves the bare
base and a full desktop:

| Step | Condition |
|---|---|
| `/proc`, `/sys`, `/dev`, `/run`, devpts, shm, `/run/user` | always |
| `fsck.ext4 -p` on root, then remount rw | if root was mounted `ro` (the installer's cmdline) |
| `sysctl -p` for `/etc/sysctl.d/*.conf` | always |
| `udevd` + `udevadm trigger` | if eudev is installed, otherwise `mdev -d` |
| modules from `/etc/modules-load.d/*.conf` | if any |
| `nft -f /etc/nftables.conf` | if nftables is installed |
| `fstrim` on every ext4 mount, 60 s after boot, in the background | if `fstrim` exists |

Everything long-running moved out of `rcS` and into supervision.

## Services

Besides the gettys, `inittab` respawns one thing:

```
::respawn:/usr/sbin/busylinux-supervise
```

a guard around `runsvdir /etc/service`. BusyBox init does **not** throttle
respawns — its own manual says so — so pointing it straight at a binary that
might be missing would spin the CPU. The guard sleeps instead.

| Service | Runs |
|---|---|
| `syslogd` | `syslogd -n -O /var/log/messages -s 2048 -b 4` |
| `klogd` | the kernel ring buffer into syslog |
| `crond` | `crond -f -c /etc/crontabs` |
| `ntpd` | `ntpd -n -p pool.ntp.org -p time.cloudflare.com` |
| `seatd` | `-g seat`, or `-g video` if that group does not exist |
| `dbus` | `dbus-daemon --system --nofork` |
| `dhcp` | `udhcpc -f` on the first Ethernet interface |

Each is a directory under `/etc/service` containing a `run` script; runit
restarts whatever exits, a second apart. All but `syslogd` and `klogd` also
carry `log/run`, which pipes the service's own output through `logger` under its
name — without it runsv sends that output to its own stdout, which ends up on
the console and in no file at all.

## Upgrading the kernel

`kernel-update.sh` does the whole thing: toolchain, source, config, build,
package, sign, index, install. Run it as root from a checkout.

```sh
apk add git
git clone https://github.com/LBarrys/busylinux.git
cd busylinux
./kernel-update.sh
```

Every toolchain package the run installs is removed again when it finishes, so
a kernel build leaves nothing behind but the kernel; anything that was already
installed is left alone. `--keep-tools` keeps them, which is what you want
alongside `--keep-source` when rebuilding.

The script is not standalone: it drives `pkgs/linux-busylinux/build` and reads
`meta` and `sha256sums` next to it, so copying it out of the tree and running it
on its own fails. It looks for the recipe beside itself, in the current
directory, in `/usr/local/share/busylinux` and in `/root/busylinux`, and
`--checkout DIR` points it anywhere else.

| Option | |
|---|---|
| `--version X.Y.Z` | kernel version, default from `pkgs/linux-busylinux/meta` |
| `--sha256 SUM` | expected checksum, for a version this tree has none for |
| `--config-only` | configure, check the symbols, stop |
| `--release N` | apk release, default: installed release + 1 |
| `--jobs N` | default `nproc` |
| `--workdir DIR` | default `/var/tmp/busylinux-kernel` |
| `--checkout DIR` | where this repository is, if not beside the script |
| `--no-vm` | leave out virtio and bochs |
| `--menuconfig` | open `menuconfig` after the fragments are merged |
| `--no-fallback` | do not keep the running kernel as `vmlinuz-previous` |
| `--no-install` | build and package only |
| `--keep-source` | reuse the build tree instead of re-extracting |
| `--keep-tools` | leave the toolchain installed afterwards |
| `--yes` | skip the confirmation |

### Moving to a different kernel version

`--version` takes any released version and the kernel.org directory follows
the major number, so `--version 7.2.7` fetches from `v7.x` and `--version
6.18.53` from `v6.x`. The release number resets to `r0` when the version
changes, and apk treats the result as an upgrade.

The tree only carries a checksum for the version in `pkgs/linux-busylinux/meta`.
For any other, take the sum from
`https://cdn.kernel.org/pub/linux/kernel/vN.x/sha256sums.asc` and pass
`--sha256`; without it, TLS is the only thing vouching for the tarball and the
script says so.

## License

The build system and the files under `pkgs/busylinux-init/` are MIT. The Linux
kernel built by `pkgs/linux-busylinux/` is GPL-2.0-only and is not distributed
here — only the recipe that fetches and configures it. Everything else in a
built image comes from Alpine Linux under its own licenses.

## Acknowledgements

- [Alpine Linux](https://alpinelinux.org/) — musl, BusyBox, apk-tools and
  everything above the kernel.
- [Limine](https://limine-bootloader.org/) — the boot loader.
- [BusyBox](https://busybox.net/) — init, the shell and most of the userland.
- Substantial parts of the build system, installer and documentation were
  written with [Claude](https://claude.ai) (Anthropic).
