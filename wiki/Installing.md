# Installing

Installing takes two steps. First the build container makes
`out/rootfs.tar.gz`: this repository's packages plus Alpine edge, installed
into a fresh root. Then `tools/install.sh` partitions a disk, unpacks the
tarball and sets up the boot loader.

## 1. Build

On any machine with Docker or Podman:

```sh
docker build -t busylinux-builder .
docker run --rm -v "$PWD/out:/build/out" -v busylinux-cache:/build/cache \
  -e HOST_UID="$(id -u)" busylinux-builder
```

The builder is a pinned `alpine:edge` image with `tools/build.sh`, `tools/lib.sh`
and `pkgs/` copied in. It builds every recipe into a signed `.apk`, indexes them,
then installs the base system from Alpine edge with those packages pinned
(`NAME@busylinux`), and packs it as `out/rootfs.tar.gz`. `HOST_UID` makes the
tarball yours rather than root's.

Options, as `-e NAME=value`:

| | |
|---|---|
| `PACKAGES="..."` | extra Alpine packages for the image, e.g. `PACKAGES="vim htop"` |
| `VM_SUPPORT=1` | adds the virtio and QEMU drivers (`pkgs/linux-busylinux/files/vm.config`) |
| `MENUCONFIG=1` | opens `make menuconfig` before the kernel builds (with `-it`) |
| `ALPINE_MIRROR=...` | another Alpine mirror |

### The cache volume

`busylinux-cache` keeps:

- `packages/x86_64/`: the built packages. A recipe is rebuilt only when its
  files, `lib.sh`, the Alpine release or `VM_SUPPORT` change. Each recipe gets
  a stamp in `stamps/`.
- `sources/`: downloaded source tarballs, checked against each recipe's `sha256sums`.
- `keys/busylinux.rsa`: **the signing key**. The installed system trusts its
  public half. If you lose it, the next build makes a new key, and a system
  installed with the old one will not take packages signed with the new one
  until you copy the new `.pub` into `/etc/apk/keys`. Keep a copy.

The first build compiles the kernel and takes a while; later builds take
minutes.

## 2. Install

Boot an Alpine live USB
in UEFI mode, with **Secure Boot off**: the kernel and Limine are not signed
for it. Get this repository and `out/rootfs.tar.gz` onto it (a second USB stick,
`wget` from another machine, or build on the live system itself). Then, as root:

```sh
apk add sgdisk dosfstools e2fsprogs efibootmgr tzdata kbd-bkeymaps
tools/install.sh --disk /dev/nvme0n1 --user alice --timezone Europe/Berlin --keymap de/de-latin1
```

`--disk` is the only required option, and **the whole disk is erased**. The
script asks you to type the disk name back unless you pass `--yes`.

| Option | |
|---|---|
| `--disk DEV` | the disk; NVMe and SATA names both work |
| `--user NAME` | a user in `video input audio seat wheel` (`wheel` may use `doas`) |
| `--hostname NAME` | default `busylinux` |
| `--timezone Zone/City` | needs `tzdata` on the live system; UTC if left out |
| `--keymap LAYOUT/VARIANT` | a BusyBox keymap, needs `kbd-bkeymaps`; US if left out |

What it does:

1. **Partitions** with GPT: a 1 GiB EFI system partition (FAT32, label
   `BUSYLINUX`) and the rest as ext4 (label `BUSYLINUX_ROOT`, partition name
   `busylinux-root`). The partition name matters: the kernel finds the root by it.
2. **Unpacks** the tarball. `/boot` is copied onto the ESP rather than unpacked,
   because FAT keeps no file modes.
3. **Configures** the hostname, `/etc/hosts`, time zone, keymap and `/etc/fstab`
   (`/tmp` is an 8 GB tmpfs).
4. **Boot loader**: Limine's `BOOTX64.EFI` at the removable-media path
   `\EFI\BOOT\BOOTX64.EFI`, so the firmware finds it even without a boot entry,
   plus an `efibootmgr` entry named BusyLinux. `limine.conf` has three entries:
   BusyLinux, BusyLinux on the serial console, and a rescue shell.
5. **Users**: adds yours, then asks for passwords for root and your user. Without
   a terminal (scripted installs), root has no password: set one after the first boot.

## The first boot

Log in on tty1. A few things to do once:

```sh
doas apk add linux-firmware-amdgpu   # the GPU needs its firmware for anything past the console
doas tools/doctor                    # from a checkout of this repository
```

Then see [Desktop](Desktop.md). The network comes up by itself on the first
wired card. Wi-Fi is not in the kernel.

## Installing on other hardware

The kernel has only this machine's drivers: AMD CPU, amdgpu, Realtek RTL8125
Ethernet, NVMe, AHCI, xHCI, USB audio. On anything else, add what it needs to
`pkgs/linux-busylinux/files/busylinux.config` first; see
[Hardware and kernel](Hardware-and-Kernel.md). In QEMU, build with `VM_SUPPORT=1`.
