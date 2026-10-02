# BusyLinux

A musl distribution on **Alpine Linux edge** with BusyBox init and runit instead
of OpenRC, and a `tinyconfig` kernel for one machine. Everything but the kernel
and init comes straight from Alpine, so its packages install unmodified.
Testing packages need their tag: `apk add electron@testing`.

## The machine

```
CPU     AMD Ryzen 7 7800X3D (Zen 4, 8C/16T)
GPU     Radeon RX 7900 GRE + Raphael iGPU       -> amdgpu
Board   MSI MAG B650 TOMAHAWK WIFI
          LAN     Realtek RTL8125BG 2.5G        -> r8169
          Audio   Realtek ALC4080               -> USB Audio
          Storage 3x NVMe, 6x SATA
RAM     32 GB DDR5
```

Other hardware: edit `pkgs/linux-busylinux/files/busylinux.config`. The build
fails if a required option is lost or an excluded one creeps back.

| Kernel extras | |
|---|---|
| Wine / Proton | `NTSYNC` |
| Xbox-protocol pads | `xpad` with rumble, `joydev` |
| Steam Input | `uinput` |
| ROCm | `HSA_AMD` with SVM |
| Containers | cgroup v2, BPF, veth, bridge, NAT, `iptables-nft` matches |
| Recovery | SysRq, REISUB only |

Left out: Wi-Fi, Bluetooth, HDMI/DP audio, swap, file systems other than ext4
and FAT32, KVM, and VM guest drivers unless built with `VM_SUPPORT=1`.

## Building

In an Alpine container (`alpine:3.24.2`, pinned by digest in the `Dockerfile`):

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
| `PACKAGES="..."` | extra Alpine packages |
| `BASE_PACKAGES="..."` | replace the default set |
| `ALPINE_BRANCH=v3.24` | a stable release instead of edge |
| `ALPINE_MIRROR=...` | default `https://mirror.maeen.sa/alpine` |
| `REPO_URL=https://...` | an extra URL for this repository |
| `LOCAL_REPO=0` | leave this repository out of the image |
| `VM_SUPPORT=1` | virtio and bochs, for QEMU |
| `MENUCONFIG=1` | `menuconfig` after merging the fragments |
| `REBUILD=1` | ignore cached packages |
| `JOBS=N` | default `nproc` |
| `IMAGE_SIZE=16G` | `disk.img` size (default 8G) |

`out/` gets `vmlinuz`, `initramfs.cpio.gz`, `amd-ucode.img`, `rootfs.tar.gz`
(for `install.sh`), `disk.img` (for QEMU) and `repo/` (signed packages and
public key). Recipes with `makedepends` build in a chroot of `ALPINE_BRANCH`.

The signing key exists only in the `busylinux-cache` volume; lose it and
installed systems reject new builds. Back it up:

```sh
docker run --rm -v busylinux-cache:/c busylinux-builder cat /c/keys/busylinux.rsa > busylinux.rsa
```

To restore, put it at `keys/busylinux.rsa` and its public half at
`keys/pub/busylinux.rsa.pub` in a fresh volume.

## QEMU

Build with `-e VM_SUPPORT=1`, then:

```sh
qemu-system-x86_64 -m 2G -nographic \
  -kernel out/vmlinuz -initrd out/initramfs.cpio.gz \
  -append "root=LABEL=BUSYLINUX_ROOT console=ttyS0" \
  -drive file=out/disk.img,format=raw,if=virtio \
  -nic user,model=virtio-net-pci
```

Log in as `root`, no password. CI runs `tests/boot-test.sh out`, which does
this unattended and checks the firewall, services, device hotplug, `update.sh`
and a power-button shutdown.

## Installing

From an Alpine live USB, as root. UEFI only; it wipes the disk and creates a
1 GiB ESP at `/boot` (kernel and Limine) and an ext4 root. Turn Secure Boot off.

```sh
apk add sgdisk dosfstools e2fsprogs parted efibootmgr
apk add tzdata kbd-bkeymaps      # for --timezone and --keymap
./install.sh --disk /dev/nvme0n1 --user alice \
  --timezone Europe/Berlin --keymap de/de-latin1
```

`--user` joins `video input audio seat wheel rtkit`; `--help` lists the rest.

## Boot and services

`rcS` mounts the pseudo file systems, efivarfs and cgroup v2, checks the root
file system, loads the keymap, sysctls and `/etc/modules-load.d`, starts the
device manager, loads the firewall and TRIMs ext4 a minute later. `rcK` saves
the clock in UTC. `/tmp` is a tmpfs of up to 8 GB.

Long-running services are runit services under `/etc/service`:

| Service | |
|---|---|
| `syslogd`, `klogd` | `/var/log/messages`, rotated at 2 MB |
| `crond` | user crontabs |
| `ntpd` | `pool.ntp.org`, `time.cloudflare.com` |
| `dhcp` | `udhcpc` on the first Ethernet interface |
| `acpid` | power button powers off |
| `seatd`, `dbus` | idle until installed |

The firewall (`/etc/nftables.conf`) drops unsolicited inbound and forwarded
traffic except ICMP, DHCP and Podman DNS; Docker and Podman bridges may forward
out and to published ports. It fails closed: with no ruleset loaded, `dhcp`
keeps the network down until `nft -f /etc/nftables.conf` succeeds.

## Desktop

```sh
apk add linux-firmware-amdgpu eudev \
    mesa-dri-gallium mesa-va-gallium mesa-vulkan-ati vulkan-loader \
    dbus rtkit seatd pipewire pipewire-alsa pipewire-pulse wireplumber \
    sway swaybg swayidle swaylock foot xwayland \
    xdg-desktop-portal xdg-desktop-portal-gtk font-noto
```

Join `seat`, log in on tty1 and run `dbus-run-session sway`, with `pipewire`,
`pipewire-pulse`, `wireplumber` and `/usr/libexec/xdg-desktop-portal` started
from the Sway config. Keep monitors on the discrete GPU.

The device manager loads modules (`amdgpu` included) and reports hotplug.
`rcS` uses the first one installed:

| Install | Runs |
|---|---|
| `mdevd libudev-zero` | `mdevd -O 4` |
| `eudev` | `udevd` |
| `libudev-zero` | BusyBox `mdev` through libudev-zero's relay |
| nothing | `mdev -d`, no module loading: use `/etc/modules-load.d` |

Take `libudev-zero` from this repository: unlike Alpine's, it shows sound
cards to PipeWire and ships the mdev relay.

`rtkit` also comes from this repository, built without polkit: it gives
PipeWire's audio threads realtime priority, which stops crackling under load,
for members of the `rtkit` group (`addgroup alice rtkit`, then log in again).
D-Bus starts it on demand.

ROCm needs `/dev/kfd`: `KERNEL=="kfd", GROUP="video", MODE="0660"` in
`/etc/udev/rules.d/70-kfd.rules` with eudev, `kfd root:video 0660` in
`/etc/mdev.conf` otherwise.

## Containers

`apk add podman` works as is. Docker needs a service:

```sh
apk add docker
mkdir -p /etc/service/docker/log
printf '#!/bin/sh\nexec 2>&1\nulimit -n 1048576\nexec dockerd\n' > /etc/service/docker/run
cp /etc/service/crond/log/run /etc/service/docker/log/run
chmod 755 /etc/service/docker/run
```

## Updating

`apk upgrade` never touches this repository's packages. Rebuild them on the
machine, as root:

```sh
git pull
./update.sh rtkit      # the named recipes; no name lists them, --all does all
./kernel-update.sh     # the kernel; the old one stays as vmlinuz-previous
```

Both sign with `/root/keys/local.rsa` (created on first use), keep changed
`/etc` files (new ones land as `.apk-new`) and remove the build tools they
installed. `kernel-update.sh --version X.Y.Z --sha256 SUM` switches release;
`--vm` adds the VM drivers. Both take `--help`.

## License

The build system and `pkgs/busylinux-init/` are MIT. The kernel is
GPL-2.0-only; only its recipe is here. Everything else comes from Alpine under
its own licenses.

## Acknowledgements

- [Alpine Linux](https://alpinelinux.org/): musl, BusyBox, apk-tools and the
  userland.
- [Limine](https://limine-bootloader.org/): the boot loader.
- [BusyBox](https://busybox.net/): init, the shell and most tools.
- Much of the build system, installer and documentation was written with
  [Claude](https://claude.ai) (Anthropic).
