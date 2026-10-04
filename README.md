# BusyLinux

A small musl system for one machine: **Alpine Linux edge** with BusyBox init and
runit instead of OpenRC, and a `tinyconfig` kernel. Everything but the kernel,
init and two patched libraries comes straight from Alpine.

```
CPU     AMD Ryzen 7 7800X3D                      GPU    Radeon RX 7900 GRE -> amdgpu
Board   MSI MAG B650 TOMAHAWK WIFI               RAM    32 GB DDR5
        LAN Realtek RTL8125BG -> r8169, audio ALC4080 -> USB Audio, NVMe + SATA
```

The kernel (`pkgs/linux-busylinux/files/busylinux.config`) adds NTSYNC, xpad,
uinput, ROCm (`HSA_AMD`), MGLRU and what Docker and Podman need. It leaves out
Wi-Fi, Bluetooth, HDMI audio, swap, KVM, file systems but ext4 and FAT, and
VM drivers unless built with `VM_SUPPORT=1`. The build fails if a required
option is lost or an excluded one comes back.

## Layout

| | |
|---|---|
| `pkgs/NAME/` | a recipe: `meta`, `build`, optional `sources` + `sha256sums`, `files/`, `scripts/` |
| `lib.sh` | builds a recipe into a signed `.apk` |
| `build.sh` | builds every recipe and `out/rootfs.tar.gz`, in the container |
| `install.sh` | installs `rootfs.tar.gz` onto a disk |
| `update.sh` | rebuilds recipes on the installed machine |
| `tests/boot-test.sh` | installs to a disk image and boots it in QEMU (CI) |

## Build and install

```sh
docker build -t busylinux-builder .
docker run --rm -v "$PWD/out:/build/out" -v busylinux-cache:/build/cache \
  -e HOST_UID="$(id -u)" busylinux-builder
```

Set `-e PACKAGES="..."` for extra Alpine packages and `-e VM_SUPPORT=1` for
QEMU. A package is rebuilt only when its recipe changes. The signing key lives
in the `busylinux-cache` volume (`keys/busylinux.rsa`); keep a copy.

From an Alpine live USB, as root, with Secure Boot off (this erases the disk):

```sh
apk add sgdisk dosfstools e2fsprogs efibootmgr tzdata kbd-bkeymaps
./install.sh --disk /dev/nvme0n1 --user alice --timezone Europe/Berlin --keymap de/de-latin1
```

## Updating

`apk upgrade` updates the Alpine packages. This repository's packages are
pinned to it (`NAME@busylinux`), so Alpine never replaces them. Rebuild them on
the machine, as root:

```sh
git pull
./update.sh                   # list the recipes
./update.sh linux-busylinux   # the kernel; the old one stays as "previous kernel"
./update.sh --all             # everything
```

A weekly workflow opens a pull request when a new kernel of the series is out,
after checking its signature. For it to work, allow GitHub Actions to create
pull requests (Settings > Actions > General). Dependabot updates the
container and the Actions.

## The system

`rcS` mounts the file systems, checks the root, loads the keymap, sysctls and
`/etc/modules-load.d`, starts the device manager and the firewall, and TRIMs
ext4 a minute later. `rcK` gives services 20 seconds to stop. runit supervises
`syslogd`, `klogd`, `crond`, `ntpd`, `acpid` (the power button powers off),
`dhcp` (the first network card) and, once installed, `seatd` and `dbus`.

The firewall (`/etc/nftables.conf`) drops unsolicited inbound and forwarded
traffic but lets Docker and Podman bridges out and published ports in. With no
ruleset loaded, `dhcp` keeps the network down.

## Desktop

```sh
apk add linux-firmware-amdgpu libudev-zero@busylinux rtkit@busylinux \
    mesa-dri-gallium mesa-va-gallium mesa-vulkan-ati vulkan-loader \
    dbus seatd pipewire pipewire-alsa pipewire-pulse wireplumber \
    sway swaybg swayidle swaylock foot xwayland \
    xdg-desktop-portal xdg-desktop-portal-gtk font-noto
```

Log in on tty1 and run `dbus-run-session sway`, starting `pipewire`,
`pipewire-pulse`, `wireplumber` and `/usr/libexec/xdg-desktop-portal` from the
Sway config.

`rcS` runs the first device manager installed: `mdevd` (with libudev-zero),
`eudev`, libudev-zero's relay for BusyBox `mdev`, or plain `mdev -d`, which
loads no modules, so list them in `/etc/modules-load.d`. This repository's
`libudev-zero` shows sound cards to PipeWire and ships the relay. Its `rtkit`
needs no polkit: members of the `rtkit` group get realtime audio threads,
which stops crackling under load.

ROCm needs `/dev/kfd`: `kfd root:video 0660` in `/etc/mdev.conf`.
Docker needs a service:

```sh
apk add docker
mkdir -p /etc/service/docker/log
printf '#!/bin/sh\nexec dockerd 2>&1\n' > /etc/service/docker/run
cp /etc/service/crond/log/run /etc/service/docker/log/
chmod 755 /etc/service/docker/run
```

## License

The build system and `pkgs/busylinux-init/` are MIT; the kernel is GPL-2.0-only.
Alpine's packages keep their own licenses. Much of this was written with
[Claude](https://claude.ai).
