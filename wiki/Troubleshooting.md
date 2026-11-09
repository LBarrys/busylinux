# Troubleshooting

Start with:

```sh
doas tools/doctor
tail -n 50 /var/log/messages
doas dmesg | tail -n 50     # dmesg is root-only
sv status /etc/service/*
```

## Boot

**Limine shows, then nothing, or the kernel cannot find the root.** The root
partition must be named `busylinux-root` (`sgdisk -p /dev/nvme0n1` shows names),
and the disk's driver must be in the kernel (NVMe or AHCI). The kernel's
messages scroll past on screen; a photo of the last ones usually names the
problem.

**"fsck.ext4 could not repair ... without help."** You get a shell. Run
`fsck.ext4 /dev/nvme0n1p2` (or what it names), answer its questions, then `exit`.

**Forgot the root password, or broke `/etc`.** Choose **Rescue shell** in
Limine, then:

```sh
mount -o remount,rw /
passwd root      # or fix the file
sync; reboot -f
```

**The screen stays at a low resolution, or goes black when amdgpu loads.**
`linux-firmware-amdgpu` is missing: install it from the console or the
rescue shell (with the network up: `udhcpc -i eth0`).

## Network

**No network.** `sv status /etc/service/dhcp`, then
`grep -E 'dhcp|udhcpc' /var/log/messages`. If it says
`no firewall ruleset is loaded`, the firewall failed to load, and that keeps
the network down on purpose: `doas nft -f /etc/nftables.conf` shows the error.
See [Network and firewall](Network-and-Firewall.md#failing-closed).

**DNS stopped working after WARP.** `/etc/resolv.conf` is locked while WARP is
on. `doas warp off` unlocks and restores it. If WARP is down but the file is
still locked: `doas chattr -i /etc/resolv.conf`.

## Packages

**`apk upgrade` wants to replace dwl (or another `pkgs/` package) with Alpine's.**
It is not pinned. `apk add dwl@busylinux`. `doctor` lists unpinned ones.

**`.busylinux-build` or `.warp-setup` in `/etc/apk/world`.** An interrupted
build or setup: `doas apk del .busylinux-build`.

**`UNTRUSTED signature` from the local repository.** The key that signed it
is not in `/etc/apk/keys`. Images built with a lost key need its new `.pub`
copied there. `update.sh` keys are added by themselves.

**`update.sh` fails in the build root.** Remove it and start fresh:
`doas rm -rf /var/tmp/busylinux-update`.

**"symbol not found" or "library not found" from a `pkgs/` program.** A
library it needs is missing. Recipes find library dependencies by
themselves now; rebuild the package with `doas tools/update.sh NAME`.

## Desktop

**dwl does not start: "Could not get seat" or no permission for `/dev/dri`.**
`seatd` must run (`apk add seatd`, then wait a minute or
`sv restart /etc/service/seatd`) and you must be in `seat` and `video`
(`id`). Log in again after joining a group.

**No sound.** `wpctl status` shows what PipeWire sees. Make sure
`pipewire`, `pipewire-pulse` and `wireplumber` all run, and that
`libudev-zero@busylinux` is installed, not Alpine's build: Alpine's hides
sound cards from PipeWire.

**Crackling sound.** Raise the minimum buffer in
`/etc/pipewire/pipewire.conf.d/10-busylinux.conf`.

**Boxes instead of Chinese, Japanese or Korean text.** No CJK font. See
[Desktop](Desktop.md#fonts).

**Programs use more memory than expected.** mimalloc; see
[Boot and init](Boot-and-Init.md#memory-mimalloc).

## Games

See the table in [Steam and games](Steam-and-Games.md#problems), and
[Controllers](Steam-and-Games.md#controllers) for pads that do not work.

**A game crashes at start, or Steam shows nothing.**

```sh
arch-chroot update
arch-chroot steam 2>&1 | tee ~/steam.log
```

For one game, set the launch option `PROTON_LOG=1 %command%`; Proton writes
`~/steam-APPID.log`. Look near the end for the first `err:`.

**Very low frame rate, `llvmpipe` in MangoHud.** The game uses software
rendering. `vulkan-radeon` and `lib32-vulkan-radeon` must be in the tree:
`arch-chroot pacman -S vulkan-radeon lib32-vulkan-radeon`.
