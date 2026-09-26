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

Beyond the hardware, the kernel carries:

| For | Options |
|---|---|
| Wine and Proton | `NTSYNC` (`/dev/ntsync`, mode 0666) |
| Xbox-protocol pads (GameSir and the like) | `JOYSTICK_XPAD` with rumble and LEDs, `INPUT_JOYDEV`, built in |
| Steam Input | `INPUT_UINPUT`, built in (`/dev/uinput`, group `input`) |
| ROCm / OpenCL | `HSA_AMD` with SVM (`/dev/kfd`, see [ROCm](#rocm)) |
| VMs | `KVM_AMD` built in (`/dev/kvm`, group `kvm`), `VHOST_NET` |
| Containers | cgroup v2 controllers, `BPF_SYSCALL`, `VETH`, `BRIDGE`, NAT and the `iptables-nft` matches Docker and Podman use |
| Recovery | `MAGIC_SYSRQ`; `kernel.sysrq=244` allows REISUB and nothing else |

Only ext4 and FAT32 (plus ISO 9660) are built in; there is no LVM, dm-crypt,
exFAT or NTFS.

Drivers built as modules are not loaded automatically at boot unless `eudev`
is installed; the base stays without it on purpose. List what you need in
`/etc/modules-load.d/*.conf` — `amdgpu`, for example — or install `eudev`.

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

### What a build produces

| In `out/` | |
|---|---|
| `vmlinuz`, `initramfs.cpio.gz` | the kernel and the (kernel-independent) initramfs |
| `amd-ucode.img` | early microcode, loaded by Limine before the initramfs |
| `disk.img` | a sparse ext4 image of the root file system, `LABEL=BUSYLINUX_ROOT`, for QEMU |
| `rootfs.tar.gz` | the same root file system, which `install.sh` unpacks |
| `busylinux.iso` | a UEFI Limine boot disc: kernel and initramfs only, it boots a root file system labelled `BUSYLINUX_ROOT` or a rescue shell; it is not a live system |
| `repo/` | this project's signed packages and the public key, for publishing as `REPO_URL` |

### The signing key

The first build generates the key that signs this project's packages and
keeps it in the `busylinux-cache` volume, nowhere else. Lose the volume and the
next build signs with a new key that installed systems do not trust. Back it
up:

```sh
docker run --rm -v busylinux-cache:/build/cache busylinux-builder \
  cat /build/cache/keys/busylinux.rsa > busylinux.rsa
chmod 600 busylinux.rsa
```

and put it back into a fresh volume before the first build there:

```sh
docker run --rm -i -v busylinux-cache:/build/cache busylinux-builder sh -ec '
  mkdir -p /build/cache/keys/pub
  cat > /build/cache/keys/busylinux.rsa
  chmod 600 /build/cache/keys/busylinux.rsa
  openssl rsa -in /build/cache/keys/busylinux.rsa -pubout \
    -out /build/cache/keys/pub/busylinux.rsa.pub' < busylinux.rsa
```

### A pinned toolchain

The `Dockerfile` pins the Arch image to a dated tag and points pacman at the
[Arch Linux Archive](https://archive.archlinux.org/) snapshot of the same day,
so the build container is the same every time. To move forward, change
`ARCH_IMAGE` and `ARCH_SNAPSHOT` together. What goes *into* the image still
follows `ALPINE_BRANCH`; edge is a moving target, a stable branch much less so.

## Running under QEMU

```sh
qemu-system-x86_64 -m 2G -nographic \
  -kernel out/vmlinuz -initrd out/initramfs.cpio.gz \
  -append "root=LABEL=BUSYLINUX_ROOT console=ttyS0" \
  -drive file=out/disk.img,format=raw,if=virtio \
  -nic user,model=virtio-net-pci
```

Log in as `root` with no password.

`tests/boot-test.sh out` does this unattended: it boots the image, logs in over
the serial console and checks the firewall, the services and a clean shutdown
from the ACPI power button. CI runs it on every push (see
[Testing](#testing)).

## Installing on real hardware

`install.sh` does the whole thing. Run it as root from an Alpine live USB — any
Linux with the tools below will do, but Alpine is musl like the target, so the
chroot at the end works without surprises.

```sh
apk add sgdisk dosfstools e2fsprogs parted efibootmgr
apk add tzdata kbd-bkeymaps      # only for --timezone and --keymap

./install.sh --disk /dev/nvme0n1 --user alice \
  --timezone Europe/Berlin --keymap de/de-latin1
```

The installer asks you to type the disk name before it touches anything, then
writes:

| Partition | Size | |
|---|---|---|
| 1 | 1 GiB | EFI system, FAT32, **mounted at `/boot`** |
| 2 | rest | root, ext4, `LABEL=BUSYLINUX_ROOT` |

| Option | |
|---|---|
| `--user NAME` | create a user in `video`, `input`, `audio`, `seat`, `wheel`, `kvm`, set a password |
| `--timezone ZONE` | e.g. `Europe/Berlin`; copies the zone file to `/etc/localtime`, so the target needs no `tzdata`. Default UTC |
| `--keymap LAYOUT/VARIANT` | console keymap from `/usr/share/bkeymaps`, e.g. `de/de-latin1`, copied to `/etc/keymap/` and loaded by `rcS`. Default US |
| `--esp-size 2G` | bigger ESP, for keeping several kernels |
| `--root-size 200G` | leave the rest of the disk unpartitioned |
| `--hostname NAME` | default `busylinux`; also written to `/etc/hosts` |
| `--no-nvram` | removable path only, do not touch the firmware boot menu |
| `--yes` | skip the confirmation |

## What rcS starts

`/etc/init.d/rcS` is deliberately conditional — the same script serves the bare
base and a full desktop:

| Step | Condition |
|---|---|
| `/proc`, `/sys`, `/dev`, `/run`, devpts, shm, `/run/user` | always |
| efivarfs on `/sys/firmware/efi/efivars` | if booted via UEFI |
| cgroup v2 on `/sys/fs/cgroup`, every controller enabled for children | always |
| `fsck.ext4 -p` on root, then remount rw | if root was mounted `ro` (the installer's cmdline) |
| keymap from `/etc/keymap/*.bmap.gz` | if one is there (`install.sh --keymap`) |
| `sysctl -p` for `/etc/sysctl.d/*.conf` | always |
| `udevd` + `udevadm trigger` | if eudev is installed, otherwise `mdev -d` |
| modules from `/etc/modules-load.d/*.conf` | if any |
| modes for `/dev/kvm` (`kvm`, 0660), `/dev/ntsync` (0666), `/dev/uinput` (`input`, 0660) | if present |
| `nft -f /etc/nftables.conf` | if the file exists — see [The firewall](#the-firewall) |
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
| `dhcp` | `udhcpc -f` on the first Ethernet interface, only once a firewall ruleset is loaded |
| `acpid` | the power button: runs `/etc/acpi/PWRF/00000080`, which powers off |

Each is a directory under `/etc/service` containing a `run` script; runit
restarts whatever exits, a second apart. All but `syslogd`, `klogd` and `acpid` also
carry `log/run`, which pipes the service's own output through `logger` under its
name — without it runsv sends that output to its own stdout, which ends up on
the console and in no file at all.

### The firewall

`/etc/nftables.conf` drops everything inbound and forwarded that is not a
reply, apart from ICMP, DHCP, and DNS and DHCP from libvirt guests and Podman
containers to the host. Forwarding is open outbound from Docker (`docker0`,
`br-*`), Podman (`podman*`) and libvirt (`virbr*`) bridges, and inbound only to
ports those tools publish.

It fails closed. `rcS` loads the ruleset before anything touches the network,
and the `dhcp` service will not bring an interface up while
`/etc/nftables.conf` exists but no ruleset is loaded; it says so in
`/var/log/messages` every 30 seconds. Fix the file, run
`nft -f /etc/nftables.conf`, and the network comes up by itself. Deleting
`/etc/nftables.conf` is the only way to run without a firewall.

## Desktop

The base is deliberately bare. A Sway desktop on this hardware:

```sh
apk add \
    linux-firmware-amdgpu eudev \
    mesa-dri-gallium mesa-va-gallium mesa-vulkan-ati \
    vulkan-loader dbus rtkit seatd \
    pipewire pipewire-alsa pipewire-pulse wireplumber \
    sway swaybg swayidle swaylock foot xwayland \
    xdg-desktop-portal xdg-desktop-portal-gtk \
    font-noto font-noto-emoji wl-clipboard grim slurp
```

`seatd` and `dbus` are already supervised; they start as soon as they are
installed.

**Install `eudev`.** Without `udevd`, `amdgpu` is not loaded at boot (see
above), and libinput — which listens for the netlink messages only `udevd`
sends — never notices a keyboard or monitor plugged in after login.

**The user must be in `seat`.** `seatd -g seat` owns the socket, and a
compositor that cannot open it dies at once with "Failed to open session".
`install.sh --user` takes care of it; by hand:

```sh
for g in video input audio seat kvm; do addgroup alice $g; done
```

Log in on tty1, not over serial or ssh, and start the session yourself;
there is no display manager:

```sh
dbus-run-session sway
```

with, in `~/.config/sway/config`:

```
exec pipewire
exec pipewire-pulse
exec wireplumber
exec /usr/libexec/xdg-desktop-portal
```

`XDG_RUNTIME_DIR` is set by `/etc/profile.d/busylinux.sh`.

A monitor on the motherboard's outputs is driven by the Raphael iGPU, a
second DRM card, and cross-GPU output is the most fragile path in every Wayland
compositor. Put the monitors on the discrete card.

### Steam and controllers

`/dev/ntsync` is there for Proton, and Xbox-protocol controllers are handled
by the built-in `xpad` driver, so they work plugged in at boot. Steam Input
writes to `/dev/uinput`, which `rcS` gives to the `input` group.

### ROCm

`HSA_AMD` exposes `/dev/kfd` once `amdgpu` is loaded. ROCm expects the user to
be able to open it; with `eudev`:

```sh
echo 'KERNEL=="kfd", GROUP="video", MODE="0660"' > /etc/udev/rules.d/70-kfd.rules
```

or, with `mdev`, add `kfd root:video 0660` to `/etc/mdev.conf`.

## Containers and VMs

The kernel has what Docker, Podman and QEMU/KVM need, `rcS` mounts cgroup v2,
and the firewall forwards for their bridges. Alpine's `iptables` is the
`iptables-nft` flavour, which is the one the kernel supports; the legacy
`ip_tables` modules are not built.

Docker, supervised like everything else:

```sh
apk add docker
mkdir -p /etc/service/docker/log
cat > /etc/service/docker/run <<'EOF'
#!/bin/sh
exec 2>&1
ulimit -n 1048576
exec dockerd
EOF
cp /etc/service/crond/log/run /etc/service/docker/log/run
chmod 755 /etc/service/docker/run
```

Podman needs no daemon: `apk add podman`. For VMs, `apk add qemu-system-x86_64`;
members of `kvm` can open `/dev/kvm`.

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

## Testing

`.github/workflows/ci.yml` runs on every push and pull request:

1. `shellcheck` over every script, at warning level, and `nft -c` over the
   firewall ruleset;
2. the full build in the pinned container, against `dl-cdn.alpinelinux.org`,
   with packages cached per change to `Dockerfile`, `build.sh` and `pkgs/`;
3. `tests/boot-test.sh` under QEMU.

The same checks run locally:

```sh
shellcheck -S warning build.sh install.sh kernel-update.sh tests/*.sh pkgs/*/build
tests/boot-test.sh out
```

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
