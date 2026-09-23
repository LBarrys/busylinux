# BusyLinux

A musl Linux distribution built on **Alpine Linux edge**, with a kernel
configured from `tinyconfig` for one specific machine and BusyBox init instead
of OpenRC.

Alpine owns the core. musl, BusyBox, apk-tools and every library come straight
from Alpine's repositories, so the ~36,000 packages in edge install and run
unmodified. This project owns three things:

- **the kernel** — built from `tinyconfig` plus a hardware fragment, not defconfig;
- **the init layer** — BusyBox init, one conditional `rcS`, and the `/etc` files
  that go with it;
- **the image** — disk image, UEFI ISO, initramfs, an installer, and a signed
  apk repository for the two packages above.

| | |
|---|---|
| Kernel | 6.18.52, **3.9 MB**, 1,172 options, 58 modules |
| Base image | **39 MB**, 21 packages |
| libc | musl 1.2.6 (Alpine's) |
| Package manager | apk-tools 3.0.8 (Alpine's), v2 and v3 repositories |
| Boot loader | Limine 12.9 (Alpine's), UEFI only |
| `/etc/os-release` | `ID=busylinux`, `ID_LIKE=alpine` |

## Contents

- [How this differs from Alpine Linux](#how-this-differs-from-alpine-linux)
- [The machine this kernel is for](#the-machine-this-kernel-is-for)
- [Repository layout](#repository-layout)
- [Building](#building)
- [Running under QEMU](#running-under-qemu)
- [Installing on real hardware](#installing-on-real-hardware)
- [Running a Wayland session](#running-a-wayland-session)
- [What rcS starts](#what-rcs-starts)
- [The kernel](#the-kernel)
- [Repositories](#repositories)
- [Adding your own packages](#adding-your-own-packages)
- [Known limits](#known-limits)
- [License](#license)
- [Acknowledgements](#acknowledgements)

## How this differs from Alpine Linux

It is Alpine edge with a different kernel and a different init, and without
Alpine's setup tooling. Anything written for Alpine applies unless it touches
`rc-*`, `setup-*`, `mkinitfs`, or the kernel package.

### The kernel

| | BusyLinux | Alpine `linux-lts` |
|---|---|---|
| vmlinuz | 3.9 MB | ~13 MB |
| modules | 58 | thousands |
| installed | 19 MB | 152 MiB |
| built from | `tinyconfig` + one hardware fragment | everything, for every machine |

The consequence that matters: **this kernel does not come from Alpine, so
`apk upgrade` will never update it.** Alpine ships kernel fixes through
`linux-lts`; here they arrive by bumping `version=` in
`pkgs/linux-busylinux/meta` and rebuilding.

### Init

Alpine's `alpine-base` pulls in `openrc`, `busybox-openrc` and
`busybox-mdev-openrc`. None of them are here. `/sbin/init` is BusyBox, driven
by `/etc/inittab`, and every service is a conditional block in one `rcS`.

`rc-update`, `rc-service`, `rc-status`, runlevels and `/etc/conf.d/*` do not
exist. Adding a daemon means editing `rcS`. `alpine-conf` is absent too, so
there is no `setup-alpine`, `setup-disk`, `update-kernel` or `lbu`.

### Boot loader

Limine, UEFI only, installed as two files on an ESP that *is* `/boot`. Alpine's
`setup-disk` uses syslinux/extlinux or GRUB and keeps `/boot` on the root
filesystem. There is no `update-extlinux` and no BIOS path.

`mkinitfs` is not used either. The initramfs here is a hand-written BusyBox one
that mounts root by label and `switch_root`s; it holds no modules, because every
driver needed to reach root is built in, so it never needs regenerating.

### What is identical

musl, BusyBox, apk-tools, `alpine-baselayout`, `mdev-conf`, `alpine-keys`, and
`/etc/apk/repositories` pointing at edge main, community and testing.

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

The ALC4080 is worth singling out: it is a USB codec on an internal port, so
`CONFIG_SND_USB_AUDIO` is what makes sound work — HDA is built only for the
display audio on the GPU. Wireless and Bluetooth are deliberately absent.

To build for different hardware, edit
`pkgs/linux-busylinux/files/busylinux.config`.

## Repository layout

```
build.sh                               everything: packages, rootfs, image, ISO
install.sh                             installs a built image onto a disk
Dockerfile                             the Arch Linux build container
pkgs/
  linux-busylinux/
    meta                               version, release, description
    sources, sha256sums                the kernel tarball
    build                              tinyconfig -> fragments -> bzImage
    files/busylinux.config             the hardware fragment
    files/vm.config                    virtio and bochs, for QEMU
  busylinux-init/
    meta, sources, build
    files/inittab                      sysinit rcS, getty on tty1-3 and ttyS0
    files/rcS, files/rcK               boot and shutdown
    files/initramfs-init               the initramfs /init
    files/busylinux.sh                 /etc/profile.d, sets XDG_RUNTIME_DIR
    files/issue, files/motd
```

The build writes to `out/`: `vmlinuz`, `initramfs.cpio.gz`, `amd-ucode.img`,
`disk.img`, `rootfs.tar.gz`, `busylinux.iso`, and `repo/`.

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
| `ALPINE_MIRROR=...` | use a closer mirror |
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

It unpacks `rootfs.tar.gz`, copies the initramfs and microcode, writes
`/etc/fstab`, drops Limine and its config into `\EFI\BOOT`, and adds a UEFI
boot entry with `efibootmgr` where it can. `\EFI\BOOT\BOOTX64.EFI` is the
removable path every firmware falls back to, so the machine still boots if its
NVRAM is cleared, and re-running the installer replaces its own old entry
rather than stacking duplicates.

The ESP *is* `/boot`, which is the point: `apk add linux-busylinux` writes the
new kernel straight to the partition the firmware reads, and `limine.conf`
already names it.

| Option | |
|---|---|
| `--user NAME` | create a user in `video`, `input`, `audio`, `seat`, set a password |
| `--esp-size 2G` | bigger ESP, for keeping several kernels |
| `--root-size 200G` | leave the rest of the disk unpartitioned |
| `--hostname NAME` | default `busylinux` |
| `--no-nvram` | removable path only, do not touch the firmware boot menu |
| `--yes` | skip the confirmation |

### In the firmware

- **Secure Boot off.** This kernel is not signed.
- **CSM off**, boot in UEFI mode. There is no BIOS path.
- EXPO stays on. Memory training is firmware-side; the kernel needs nothing.

### After the first boot

```sh
apk update
apk add linux-firmware-amdgpu linux-firmware-rtl_nic
```

Those are the only two firmware packages this kernel ever asks for. Without the
first the GPU falls back to a bare framebuffer; the second is the
`rtl_nic/rtl8125b-2.fw` patch the built-in `r8169` requests when the link comes
up. Reboot afterwards — `amdgpu` binds at boot and will not pick up firmware
that appeared later.

## Running a Wayland session

There is no display manager and no OpenRC. `rcS` has already started `seatd`,
the system D-Bus and `udevd`; the rest belongs to the session. Log in **on
tty1**, not over serial or ssh — the TTY backend needs a real virtual terminal
to take over — then:

```sh
dbus-run-session sway
```

`XDG_RUNTIME_DIR` is set by `/etc/profile.d/busylinux.sh`.

Two things bite, and both are silent.

**`eudev` is not optional.** `apk` pulls in `eudev-libs` on its own, because
libinput links against `libudev.so.1` — but that is the *library*, not the
daemon. Without `eudev` there is no `udevd`, and `libudev`'s monitor listens on
the netlink group that only `udevd` broadcasts on. Enumeration at startup still
works off sysfs, so a compositor comes up; nothing plugged in afterwards is
ever noticed. A monitor or keyboard connected after login simply does not
appear. `rcS` also falls back to `mdev -d` when `udevd` is absent.

**The user must be in `seat`.** `rcS` runs `seatd -g seat`, and the socket is
`srwxrwx--- root:seat`. A compositor that cannot open it dies immediately:

```
thread 'main' panicked at src/main.rs:186:6:
called `Result::unwrap()` on an `Err` value: error initializing the TTY backend
Caused by:
    0: Error creating a session.
    1: Failed to open session: Function not implemented (os error 38)
```

`install.sh --user NAME` adds all four groups. By hand:

```sh
adduser alice
for g in video input seat audio; do addgroup alice $g; done
```

## What rcS starts

`/etc/init.d/rcS` is deliberately conditional — the same script serves the bare
base and a full desktop:

| Step | Condition |
|---|---|
| `/proc`, `/sys`, `/dev`, `/run`, devpts, shm, `/run/user` | always |
| `syslogd -C512` and `klogd` | always (read with `logread`) |
| `udevd` + `udevadm trigger` | if eudev is installed, otherwise `mdev -d` |
| modules from `/etc/modules-load.d/*.conf` | if any |
| `seatd -g seat` (or `-g video`) | if seatd is installed |
| `dbus-daemon --system` | if dbus is installed |
| `udhcpc` on every Ethernet interface | always |

### mdev needs a device table

BusyBox `mdev` has no built-in one. Its default rule is `root:root 0660`, and
`mdev -d` rescans all of `/sys` when it starts, so it overwrites the modes the
kernel gave devtmpfs — including `/dev/null`, which stops being writable by
anyone but root:

```
-sh: /etc/profile.d/busylinux.sh: line 5: can't create /dev/null: Permission denied
```

Nearly everything an ordinary user runs breaks in some form after that; `git`
is a good example, since it writes to `/dev/null` constantly. Alpine's
`mdev-conf` package is therefore part of the base. It restores the usual modes
and does more besides: `@modprobe -q -b "$MODALIAS"` loads a driver when a
device appears, and `/lib/mdev/persistent-storage` builds `/dev/disk/by-*`.
Install `eudev` and `rcS` uses `udevd` instead, which brings its own rules.

## The kernel

`pkgs/linux-busylinux/files/busylinux.config` is a Kconfig fragment merged onto
`make tinyconfig`. Every line is something the machine above needs; nothing
else is on. Adding hardware means adding a line, then bumping `release=` in
`pkgs/linux-busylinux/meta`:

```sh
echo 'CONFIG_BTRFS_FS=m' >> pkgs/linux-busylinux/files/busylinux.config
```

`pkgs/linux-busylinux/files/vm.config` adds virtio and bochs so the same kernel
boots under QEMU for testing. `VM_SUPPORT=0` drops it.

### tinyconfig hides things, so the build checks

Starting from nothing means an option whose dependency is missing is dropped
**silently**. The recipe therefore asserts that ~40 symbols survived
`olddefconfig` and fails the build otherwise. Three that were found this way,
all EXPERT-gated and all off in `tinyconfig`:

| Symbol | Without it |
|---|---|
| `CONFIG_TTY` | no virtual terminals *and* no serial console — a silent machine |
| `CONFIG_FILE_LOCKING` | `apk` cannot lock its database: "Function not implemented" |
| `CONFIG_MEMFD_CREATE` | Wayland buffers, PipeWire and Mesa fail in obscure ways |

If the build stops with `config lost: FOO`, that option's dependency is missing
— add it too rather than deleting the check.

A symbol left out is not the same as a symbol turned off. `CONFIG_WLAN` is a
menu bool that defaults to `y`, so omitting it let `olddefconfig` put it back
along with every vendor submenu. Forcing it off takes an explicit
`# CONFIG_WLAN is not set` line in the fragment, which is why a few of those
appear there.

## Repositories

`/etc/apk/repositories` in the image:

```
v3 /var/lib/busylinux/repo
https://dl-cdn.alpinelinux.org/alpine/edge/main
https://dl-cdn.alpinelinux.org/alpine/edge/community
@testing https://dl-cdn.alpinelinux.org/alpine/edge/testing
```

`testing` is **tagged**: it is never used unless named, so a stray testing
build cannot replace something from community. `apk add wiremix@testing` opts
in for that one package.

Alpine's repositories are v2 and this one is v3; apk-tools 3 reads both, which
is why they sit in the same file.

### Why the image carries its own repository

The kernel and `busylinux-init` exist in no Alpine repository, so without
somewhere to find them apk treats them as orphans and says so on every
interactive run:

```
NOTE: Consider running apk upgrade with --prune and/or --available.
The following packages are no longer available from a repository:
  busylinux-init linux-busylinux
```

**Do not take that advice.** `apk upgrade --prune` purges exactly those two
packages — the kernel and `/sbin/init` — and the machine will not boot again.

The build therefore copies the current release of each of its own packages into
`/var/lib/busylinux/repo` inside the image (7.4 MB) and puts that path first in
`/etc/apk/repositories`. apk reads a plain filesystem path as a repository, so
nothing has to be served. `LOCAL_REPO=0` turns this off.

## Adding your own packages

`pkgs/<name>/` holds `meta`, `sources`, `sha256sums`, `build`, and optionally
`files/`, `patches/` and `split/`. `meta` is a shell fragment with `version`,
`release`, `desc`, `depends`, `replaces` and `options`. The build script
receives the destination directory as `$1`:

```sh
#!/bin/sh -e
make
make DESTDIR="$1" install
```

Recipes are built with the **container's** toolchain, which is right for the
kernel and for anything freestanding. There is no musl sysroot here, so a
package that links against libc should come from Alpine instead — that is the
whole point of letting Alpine own the core.

`busylinux-init` shows how to take ownership of a file Alpine already ships: it
sets `replaces="alpine-baselayout-data alpine-baselayout"` so its `/etc/inittab`
and `/etc/motd` can replace theirs.

```sh
recipes="-v $PWD/pkgs:/usr/local/share/busylinux/pkgs"
docker run --rm $recipes -v busylinux-cache:/build/cache \
  busylinux-builder build.sh --checksum mypkg
docker run --rm -it $recipes -v "$PWD/out:/build/out" \
  -v busylinux-cache:/build/cache busylinux-builder
```

## Known limits

- **The hardware paths are configured from specifications, not measured.**
  Everything in this tree is verified under QEMU — boot, install, apk, the
  compositor. The amdgpu, r8169 and ALC4080 drivers cannot be exercised there.
  Boot in a VM first, then from a USB stick, before installing. `lspci -k` and
  `lsusb -t` confirm which drivers bind.
- **The kernel is not covered by `apk upgrade`.** Security fixes mean a rebuild.
- **No Wi-Fi and no Bluetooth.** Wired Ethernet only, by choice.
- **No display manager, no OpenRC.** Session services are your responsibility.
- **Flatpak needs its own runtime.** It is glibc-based and self-contained, so
  it works on musl — the usual way to run software Alpine does not package.

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
