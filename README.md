# BusyLinux

A musl Linux distribution built on **Alpine Linux edge**, with a kernel
configured from `tinyconfig` for one specific machine and BusyBox init instead
of OpenRC. musl, BusyBox, apk-tools and every library come straight from
Alpine, so the ~36,000 packages in edge install and run unmodified. The
testing repository is configured but tagged, so its packages need the tag:
`apk add electron@testing`.

## The machine

```
CPU     AMD Ryzen 7 7800X3D (Zen 4, AM5, 8C/16T)
GPU     Radeon RX 7900 GRE (Navi 31) + Raphael iGPU  -> amdgpu
Board   MSI MAG B650 TOMAHAWK WIFI
          LAN     Realtek RTL8125BG 2.5G             -> r8169
          Audio   Realtek ALC4080                    -> USB Audio Class, not HDA
          Storage 3x NVMe, 6x SATA
RAM     32 GB DDR5, EXPO (firmware-side; the kernel needs nothing for it)
```

For other hardware, edit `pkgs/linux-busylinux/files/busylinux.config`. The
build fails if a symbol it relies on is lost or one it excludes creeps back.

The kernel also carries:

| For | |
|---|---|
| Wine / Proton | `NTSYNC` |
| Xbox-protocol pads (GameSir and the like) | `xpad` with rumble, `joydev`, built in |
| Steam Input | `uinput`, built in |
| ROCm | `HSA_AMD` with SVM |
| VMs | `KVM_AMD` built in, `vhost-net` |
| Containers | cgroup v2, BPF, veth, bridge, NAT, the `iptables-nft` matches |
| Recovery | SysRq, limited to REISUB |

Left out on purpose: Wi-Fi, Bluetooth, and every file system but ext4 and
FAT32.

Modules are not loaded automatically at boot unless `eudev` is installed; the
base stays without it. List what you need in `/etc/modules-load.d/*.conf`
(`amdgpu`, for one) or install `eudev`.

## Building

Everything is compiled inside an Arch Linux container:

```sh
docker build -t busylinux-builder .
mkdir -p out
docker run --rm -it \
  -v "$PWD/out:/build/out" -v busylinux-cache:/build/cache \
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
  busylinux-builder
```

| Variable | |
|---|---|
| `PACKAGES="..."` | extra Alpine packages for the image |
| `BASE_PACKAGES="..."` | replace the default package set |
| `ALPINE_BRANCH=v3.24` | a stable release instead of edge |
| `ALPINE_MIRROR=...` | default `https://mirror.maeen.sa/alpine` |
| `REPO_URL=https://...` | an extra network URL for this repository |
| `LOCAL_REPO=0` | do not copy this repository into the image |
| `VM_SUPPORT=0` | leave out virtio and bochs |
| `MENUCONFIG=1` | open `menuconfig` after the fragments are merged |
| `REBUILD=1` | rebuild packages even when cached |
| `JOBS=N` | default `nproc` |
| `IMAGE_SIZE=16G` | size of the sparse `disk.img` (default 8G) |

`out/` then holds `vmlinuz`, `initramfs.cpio.gz` and `amd-ucode.img`;
`rootfs.tar.gz`, which `install.sh` unpacks; `disk.img`, the same root file
system as an ext4 image for QEMU; and `repo/`, this project's signed packages
and public key.

The signing key lives only in the `busylinux-cache` volume. Lose it and
installed systems stop trusting new builds, so keep a copy:

```sh
docker run --rm -v busylinux-cache:/c busylinux-builder cat /c/keys/busylinux.rsa > busylinux.rsa
```

To restore it into a fresh volume, put it back at `keys/busylinux.rsa` and its
public half (`openssl rsa -pubout`) at `keys/pub/busylinux.rsa.pub` before the
first build.

The `Dockerfile` pins the Arch image and the matching
[Arch Linux Archive](https://archive.archlinux.org/) snapshot; move
`ARCH_IMAGE` and `ARCH_SNAPSHOT` together.

## Running under QEMU

```sh
qemu-system-x86_64 -m 2G -nographic \
  -kernel out/vmlinuz -initrd out/initramfs.cpio.gz \
  -append "root=LABEL=BUSYLINUX_ROOT console=ttyS0" \
  -drive file=out/disk.img,format=raw,if=virtio \
  -nic user,model=virtio-net-pci
```

Log in as `root` with no password. `tests/boot-test.sh out` does the same
unattended and checks the firewall, the services and a clean power-button
shutdown; CI runs it, after shellcheck and the full build, on every pull
request and push to `main`.

## Installing

Run `install.sh` as root from an Alpine live USB. It is UEFI only, asks you to
type the disk name, then wipes the disk and writes a 1 GiB ESP (FAT32, mounted
at `/boot`, holding the kernel and Limine) and an ext4 root.

```sh
apk add sgdisk dosfstools e2fsprogs parted efibootmgr
apk add tzdata kbd-bkeymaps      # only for --timezone and --keymap
./install.sh --disk /dev/nvme0n1 --user alice \
  --timezone Europe/Berlin --keymap de/de-latin1
```

`--user` adds the user to `video input audio seat wheel kvm`; `--help` lists
the rest. Turn Secure Boot off: the kernel is not signed.

## Boot and services

`/etc/init.d/rcS` mounts the pseudo file systems, efivarfs and cgroup v2;
checks the root file system if it was mounted read-only; loads the keymap,
sysctls and `/etc/modules-load.d`; starts `udevd` if eudev is installed and
`mdev` otherwise; sets the modes of `/dev/kvm` (group `kvm`), `/dev/ntsync`
and `/dev/uinput` (group `input`); loads the firewall; and TRIMs every ext4
file system a minute later. `rcK` writes the clock to the RTC (in UTC) at
shutdown.

Everything long-running is a runit service under `/etc/service`, started by
`inittab` through a guard that sleeps rather than let BusyBox init respawn a
missing binary in a tight loop:

| Service | |
|---|---|
| `syslogd`, `klogd` | into `/var/log/messages`, rotated at 2 MB |
| `crond` | for user crontabs; root's is empty |
| `ntpd` | `pool.ntp.org`, `time.cloudflare.com` |
| `dhcp` | `udhcpc` on the first Ethernet interface |
| `acpid` | the power button powers off |
| `seatd`, `dbus` | idle until installed |

Services other than `syslogd`, `klogd` and `acpid` log through `logger` under
their own name.

### The firewall

`/etc/nftables.conf` drops everything inbound and forwarded that is not a
reply, except ICMP, DHCP, and DNS/DHCP from libvirt and Podman guests to the
host. Docker, Podman and libvirt bridges may forward outbound, and to ports
those tools publish.

It fails closed: while `/etc/nftables.conf` exists and no ruleset is loaded,
`dhcp` brings no interface up and says so in the log. Fix the file and run
`nft -f /etc/nftables.conf`; the network follows within 30 seconds.

## Desktop

```sh
apk add linux-firmware-amdgpu eudev \
    mesa-dri-gallium mesa-va-gallium mesa-vulkan-ati vulkan-loader \
    dbus rtkit seatd pipewire pipewire-alsa pipewire-pulse wireplumber \
    sway swaybg swayidle swaylock foot xwayland \
    xdg-desktop-portal xdg-desktop-portal-gtk font-noto
```

`eudev` is what loads `amdgpu` and lets libinput see devices plugged in after
login. The user must be in `seat`. Log in on tty1 and run
`dbus-run-session sway`, with `exec pipewire`, `exec pipewire-pulse`,
`exec wireplumber` and `exec /usr/libexec/xdg-desktop-portal` in the Sway
config. Keep monitors on the discrete GPU; the iGPU is a second DRM card, and
cross-GPU output is fragile in every Wayland compositor.

ROCm needs access to `/dev/kfd`: with eudev,
`KERNEL=="kfd", GROUP="video", MODE="0660"` in `/etc/udev/rules.d/70-kfd.rules`;
with mdev, `kfd root:video 0660` in `/etc/mdev.conf`.

## Containers and VMs

`apk add podman`, or `qemu-system-x86_64` (members of `kvm` can use
`/dev/kvm`), works as is. Docker needs a service:

```sh
apk add docker
mkdir -p /etc/service/docker/log
printf '#!/bin/sh\nexec 2>&1\nulimit -n 1048576\nexec dockerd\n' > /etc/service/docker/run
cp /etc/service/crond/log/run /etc/service/docker/log/run
chmod 755 /etc/service/docker/run
```

## Upgrading the kernel

From a checkout, as root:

```sh
./kernel-update.sh
```

It installs the toolchain, builds the kernel from `pkgs/linux-busylinux`,
signs and installs the package, and removes the toolchain again. It keeps the
running kernel as `vmlinuz-previous`. `--version X.Y.Z --sha256 SUM` moves to
another release (the sum is in kernel.org's `sha256sums.asc`); `--help` lists
the rest.

## License

The build system and `pkgs/busylinux-init/` are MIT. The kernel built by
`pkgs/linux-busylinux/` is GPL-2.0-only and not distributed here, only the
recipe. Everything else in an image comes from Alpine under its own licenses.

## Acknowledgements

- [Alpine Linux](https://alpinelinux.org/) — musl, BusyBox, apk-tools and
  everything above the kernel.
- [Limine](https://limine-bootloader.org/) — the boot loader.
- [BusyBox](https://busybox.net/) — init, the shell and most of the userland.
- Substantial parts of the build system, installer and documentation were
  written with [Claude](https://claude.ai) (Anthropic).
