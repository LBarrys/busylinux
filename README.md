# BusyLinux

A small musl system for one machine: **Alpine Linux edge** with BusyBox init and
runit instead of OpenRC, and a `tinyconfig` kernel. Everything but the kernel,
init, two patched libraries, nftables and wgcf comes straight from Alpine.

```
CPU     AMD Ryzen 7 7800X3D                      GPU    Radeon RX 7900 GRE -> amdgpu
Board   MSI MAG B650 TOMAHAWK WIFI               RAM    32 GB DDR5
        LAN Realtek RTL8125BG -> r8169, audio ALC4080 -> USB Audio, NVMe + SATA
```

The kernel (`pkgs/linux-busylinux/files/busylinux.config`) adds NTSYNC, xpad,
uinput, ROCm (`HSA_AMD`), MGLRU, zram and what Docker and Podman need. It leaves
out Wi-Fi, Bluetooth, HDMI audio, KVM, file systems but ext4 and FAT, and
VM drivers unless built with `VM_SUPPORT=1`. The build fails if a required
option is lost or an excluded one comes back.

## Layout

| | |
|---|---|
| `pkgs/NAME/` | a recipe: `meta`, `build`, optional `sources` + `sha256sums`, `files/`, `scripts/` |
| `tools/lib.sh` | builds a recipe into a signed `.apk` |
| `tools/build.sh` | builds every recipe and `out/rootfs.tar.gz`, in the container |
| `tools/install.sh` | installs `rootfs.tar.gz` onto a disk |
| `tools/update.sh` | rebuilds recipes on the installed machine |
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
tools/install.sh --disk /dev/nvme0n1 --user alice --timezone Europe/Berlin --keymap de/de-latin1
```

## Updating

`apk upgrade` updates the Alpine packages. This repository's packages are
pinned to it (`NAME@busylinux`), so Alpine never replaces them. Rebuild them on
the machine, as root:

```sh
git pull
tools/update.sh                   # list the recipes
tools/update.sh linux-busylinux   # the kernel
tools/update.sh --all             # everything
```

A weekly workflow opens a pull request when a new kernel of the series is out,
after checking its signature. CI keeps each kernel it compiles in the `kernel-cache`
release, so later runs on any branch reuse it instead of compiling again. For it to work, allow GitHub Actions to create
pull requests (Settings > Actions > General). Dependabot updates the
container and the Actions.

## The system

Limine starts the kernel and the AMD microcode, with no initramfs; the kernel
mounts the partition named `busylinux-root` read-only. The "Rescue shell" entry
starts `/bin/sh` instead of init (`mount -o remount,rw /` to change files).
`rcS` mounts the file systems, checks the root, loads the keymap, sets up zram
swap (half the RAM, zstd), loads sysctls and `/etc/modules-load.d`, starts the device manager and the firewall, TRIMs
ext4 a minute later and last runs `/etc/rc.local`, yours to fill. `rcK` gives services 20 seconds to stop. runit supervises
`syslogd`, `klogd`, `crond`, `ntpd`, `acpid` (the power button powers off),
`dhcp` (the first network card) and, once installed, `seatd` and `dbus`.
Services and login sessions allocate memory with mimalloc in secure mode
(`LD_PRELOAD`, as musl has no `ld.so.preload`); `doas` and setuid programs keep
musl's own. The network card's IPv6 addresses come from random bits, not its
MAC, and DHCP requests do not name the OS.

The firewall (`/etc/nftables.conf`, with this repository's `nftables`: no GNU
GMP or Readline, so no `nft -i`) drops unsolicited inbound and forwarded
traffic but lets Docker and Podman bridges out and published ports in. With no
ruleset loaded, `dhcp` keeps the network down.

## Desktop

```sh
apk add linux-firmware-amdgpu libudev-zero@busylinux \
    mesa-dri-gallium mesa-va-gallium mesa-vulkan-ati vulkan-loader \
    dbus seatd pipewire pipewire-alsa pipewire-pulse wireplumber \
    sway swaybg swayidle swaylock foot xwayland \
    xdg-desktop-portal xdg-desktop-portal-gtk font-noto
```

Log in on tty1 and run `dbus-run-session sway`, starting `pipewire`,
`pipewire-pulse`, `wireplumber` and `/usr/libexec/xdg-desktop-portal` from the
Sway config.

`rcS` runs `mdevd` as the device manager, with `/etc/mdev.conf`, which also
loads modules; it passes each event on to libudev-zero (`-O 4`), as Alice Linux
does. This repository's `libudev-zero` shows sound cards to PipeWire. Logins on
tty1-3 may use realtime priority (`/etc/inittab`), so PipeWire's audio threads
get it without rtkit.
`/etc/pipewire/pipewire.conf.d/10-busylinux.conf` keeps PipeWire's buffers at
1024 samples or more, below which the board's USB audio crackles.

ROCm needs `/dev/kfd`: `kfd root:video 0660` in `/etc/mdev.conf`.
Docker needs a service:

```sh
apk add docker
mkdir -p /etc/service/docker/log
printf '#!/bin/sh\nexec dockerd 2>&1\n' > /etc/service/docker/run
cp /etc/service/ntpd/log/run /etc/service/docker/log/
chmod 755 /etc/service/docker/run
```

To use `docker` without `doas`, join its group (`doas addgroup alice docker`)
and log in again. Membership amounts to root: a container can mount `/`.

## Steam and Windows games

`/etc/sysctl.d/20-games.conf` and the logins' file limit (`/etc/inittab`) are
set for Proton already. Steam and Proton need glibc and 32-bit libraries,
which Alpine (musl) lacks.
They run from an Arch Linux tree in `/opt/root.x86_64`, entered with
`bubblewrap` (no container engine). `tools/arch-setup`, run once as your user,
downloads and checks the tree, enables multilib, installs Steam, umu-launcher,
MangoHud and the Radeon Vulkan drivers, and puts `arch-chroot` in
`~/.local/bin` with a Steam entry for launchers. Root steps go through `doas`.

```sh
tools/arch-setup
arch-chroot update                      # later: pacman -Syu
arch-chroot pacman -S vulkan-tools      # more packages
```

Root in the tree gets `--cap-add ALL`, so pacman can switch to its download
user, which Landlock then confines to the download folder. Programs run as you,
with your home, drives under `/mnt` and `/media`, devices, display and
PipeWire, and without `LD_PRELOAD`: the tree's glibc cannot load musl's
mimalloc.

Then `arch-chroot steam`, or for a game outside Steam
`WINEPREFIX=~/Games/name/prefix GAMEID=0 arch-chroot umu-run ~/Games/name/Game.exe`.
The Steam entry is `~/.local/share/applications/steam.desktop`; `Icon=steam`
comes from the icon theme, such as Papirus.

MangoHud goes into the tree, as games load the tree's libraries. Test it with
`arch-chroot mangohud vkcube` (from `vulkan-tools`), then use `MANGOHUD=1 %command%`
as a game's launch option. In `~/.config/MangoHud/MangoHud.conf`, leave out
`font_file`: Steam runs games in its own runtime, where host font paths do not
exist, and a missing font stops the game. `fps_limit=60` keeps the CPU cooler.

The Steam client is an X11 program: dwl needs Xwayland, or run
`xwayland-satellite`. Proton's "unable to use parent for game drive" is only a
notice; keep each game in a folder of its own. "open /dev/snd/seq failed" means
no MIDI: the kernel has no ALSA sequencer.

## Cloudflare WARP

Cloudflare's own client needs glibc and policy routing, which the kernel leaves
out. WARP is WireGuard, though: `wgcf` (`pkgs/wgcf`, newer than Alpine's) makes
a profile and `tools/warp` brings it up with `wg` and `ip` alone (no bash
`wg-quick`), as `warp`:

```sh
tools/warp-setup     # wgcf, wg, an account and /etc/wireguard/wgcf-profile.conf
doas warp on         # all traffic and DNS through Cloudflare
wget -qO- https://www.cloudflare.com/cdn-cgi/trace | grep warp   # warp=on
doas warp off
```

`tools/warp-setup KEY` binds a WARP+ license key and makes the profile anew.

It routes `0.0.0.0/1` and `128.0.0.0/1` (and IPv6 halves) into the tunnel, so
DHCP renewals, which only replace the default route, leave it alone, and locks
`/etc/resolv.conf` to Cloudflare's DNS until `warp off`. It is not a kill
switch: if the tunnel drops, traffic goes out directly.

## License

The build system and `pkgs/busylinux-init/` are MIT; the kernel is GPL-2.0-only.
Alpine's packages keep their own licenses. Much of this was written with
[Claude](https://claude.ai).
