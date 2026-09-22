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
| Kernel | 6.18.52, **3.9 MB**, 1,289 options, 92 modules |
| Base image | **43 MB**, 21 packages |
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
| modules | 92 | thousands |
| installed | 21 MB | 152 MiB |
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
exist. Adding a daemon means editing `rcS`.

### Missing Alpine tooling

`alpine-conf` is not installed, so none of `setup-alpine`, `setup-disk`,
`setup-interfaces`, `setup-xorg-base`, `setup-wayland-base`, `update-kernel`,
`lbu` or the apkovl mechanism are available. Neither is `alpine-release`, so
there is no `/etc/alpine-release`; `/etc/os-release` comes from
`busylinux-init`.

`mkinitfs` is not used either. Alpine regenerates its initramfs on every kernel
upgrade through apk triggers. The initramfs here is a hand-written BusyBox one
that mounts root by label and `switch_root`s; it contains no modules, because
every driver needed to reach root is built in, so it never needs regenerating.

### Boot loader

Limine, UEFI only, installed as two files on an ESP that *is* `/boot`. Alpine's
`setup-disk` uses syslinux/extlinux or GRUB and keeps `/boot` on the root
filesystem. There is no `update-extlinux` and no BIOS path.

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
          Wi-Fi   MediaTek MT7922 802.11ax           -> mt7921e (PCIe)
          BT      MT7922 companion controller        -> btusb + btmtk (USB)
          Storage 3x NVMe, 6x SATA
RAM     32 GB DDR5, EXPO (firmware-side; the kernel needs nothing for it)
```

Two devices on this board are USB devices that do not look like USB devices.
The ALC4080 is a USB codec on an internal port, so `CONFIG_SND_USB_AUDIO` is
what makes sound work — HDA is built only for the display audio on the GPU. The
MT7922 is split: its Wi-Fi half is PCIe and binds `mt7921e`, while its
Bluetooth half is a USB device and needs `btusb` with
`CONFIG_BT_HCIBTUSB_MTK=y`, which pulls in `btmtk`. Both halves want
`linux-firmware-mediatek`.

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

docker build -t busylinux-builder .
mkdir -p out
docker run --rm -it \
  -v "$PWD/out:/build/out" -v busylinux-cache:/build/cache \
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
  busylinux-builder
```

**Keep the `busylinux-cache` volume.** It holds the kernel source, the built
packages and this repository's private signing key.

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

`out/busylinux.iso` is a 9 MB UEFI boot medium for the same kernel — Limine,
the kernel and the initramfs, nothing else. It is useful for booting a machine
whose ESP has gone wrong. It is **not** a live system: it looks for a disk
labelled `BUSYLINUX_ROOT` and drops to the rescue shell if there is none.

## Installing on real hardware

`install.sh` does the whole thing. Run it as root from an Alpine live USB — any
Linux with the tools below will do, but Alpine is musl like the target, so the
chroot at the end works without surprises.

```sh
apk add sgdisk dosfstools e2fsprogs parted efibootmgr

./install.sh --disk /dev/nvme0n1 --user alice
```

There is no boot loader in that list. Limine on UEFI is an EFI application and
a text file, and its `BOOTX64.EFI` comes out of the root filesystem that was
just unpacked, so the machine you install *from* needs only the partitioning
tools.

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
apk add linux-firmware-amdgpu linux-firmware-mediatek
```

Without those the GPU falls back to a bare framebuffer and the MT7922 does not
associate. Reboot afterwards — `amdgpu` binds at boot and will not pick up
firmware that appeared later.

### Installing by hand

Partition and format, then:

```sh
mount /dev/nvme0n1p2 /mnt && mkdir /mnt/boot && mount /dev/nvme0n1p1 /mnt/boot
tar -xpf out/rootfs.tar.gz -C /mnt --numeric-owner
cp out/initramfs.cpio.gz out/amd-ucode.img /mnt/boot/
mkdir -p /mnt/boot/EFI/BOOT
cp /mnt/usr/share/limine/BOOTX64.EFI /mnt/boot/EFI/BOOT/
```

with `/mnt/boot/EFI/BOOT/limine.conf`:

```
timeout: 3

/BusyLinux
    protocol: linux
    path: boot():/vmlinuz-busylinux
    cmdline: root=LABEL=BUSYLINUX_ROOT rw
    module_path: boot():/amd-ucode.img
    module_path: boot():/initramfs.cpio.gz
```

`boot():` is the partition the config was read from, so paths are relative to
the ESP. Modules are handed to the kernel in the order given, and the microcode
must be **first**: the kernel reads it before it does anything else with the
CPU. Limine looks for its config beside its own EFI application before anywhere
else, so `\EFI\BOOT\limine.conf` cannot be shadowed by a stray config on
another partition.

The root filesystem is found by label. If another disk in the machine also
carries `BUSYLINUX_ROOT`, the initramfs takes whichever it finds first —
relabel one with `e2label`, or use `root=UUID=`.

### The two that bite

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

Log in **on tty1**, not over serial or ssh: the TTY backend needs a real
virtual terminal to take over.

### Starting a session

There is no display manager and no OpenRC. `rcS` has already started `seatd`,
the system D-Bus and `udevd`; the rest belongs to the session:

```sh
dbus-run-session sway
```

and in `~/.config/sway/config`:

```
exec pipewire
exec pipewire-pulse
exec wireplumber
exec /usr/libexec/xdg-desktop-portal &
```

`XDG_RUNTIME_DIR` is set by `/etc/profile.d/busylinux.sh`.

### Two GPUs

A monitor plugged into the motherboard's HDMI or DisplayPort is driven by the
Raphael iGPU, which is a second DRM card. Cross-GPU output is the most fragile
path in every Wayland compositor. Putting both monitors on the discrete card
avoids it entirely. `ls /sys/class/drm/*/status` shows which connectors are
attached to which card.

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
| `bluetoothd` | if bluez is installed and dbus is running |
| `udhcpc` on every Ethernet and Wi-Fi interface | always |

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
else is on. Adding hardware means adding a line:

```sh
echo 'CONFIG_BTRFS_FS=m' >> pkgs/linux-busylinux/files/busylinux.config
# bump release= in pkgs/linux-busylinux/meta, then rebuild
```

`pkgs/linux-busylinux/files/vm.config` adds virtio and bochs so the same kernel
boots under QEMU for testing. `VM_SUPPORT=0` drops it.

### tinyconfig hides things, so the build checks

Starting from nothing means an option whose dependency is missing is dropped
**silently**. The recipe therefore asserts that ~45 symbols survived
`olddefconfig` and fails the build otherwise. Three that were found this way,
all EXPERT-gated and all off in `tinyconfig`:

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
`/var/lib/busylinux/repo` inside the image (about 8.4 MB) and puts that path
first in `/etc/apk/repositories`. apk reads a plain filesystem path as a
repository, so nothing has to be served. `LOCAL_REPO=0` turns this off.

To fix an image built before this existed, without rebuilding:

```sh
mkdir -p /var/lib/busylinux/repo
cp -r out/repo/x86_64 /var/lib/busylinux/repo/
cp out/repo/busylinux.rsa.pub /etc/apk/keys/     # if it is not there yet
sed -i '\|^#v3 |c\v3 /var/lib/busylinux/repo' /etc/apk/repositories
```

### Over the network instead

The same repository is published to `out/repo` on every build, signed with a
key generated on the first build and kept in the cache volume. Serve it and set
`REPO_URL=` to add a second line, so machines can pull kernel updates:

```sh
cd out/repo && python3 -m http.server 8080
```

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
  compositor. The amdgpu, r8169, mt7921e, btusb and ALC4080 drivers cannot be
  exercised there. Boot in a VM first, then from a USB stick, before
  installing. `lspci -k` and `lsusb -t` confirm which drivers bind.
- **The kernel is not covered by `apk upgrade`.** Security fixes mean a
  rebuild.
- **Wireless needs firmware and userspace.** The kernel side is built in, but
  `mt7921e` will not associate without `linux-firmware-mediatek`, and the base
  image has no `wpa_supplicant`, `iw` or `bluetoothd`.
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
- Substantial parts of the build system, installer and documentation were written with [Claude](https://claude.ai) (Anthropic).
