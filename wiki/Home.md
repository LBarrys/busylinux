# BusyLinux wiki

BusyLinux is a small musl system for one machine: Alpine Linux edge, with BusyBox
init and runit in place of OpenRC, a kernel built from `tinyconfig`, and no
initramfs. Eleven packages are built from this repository (`pkgs/`). Everything
else is plain Alpine, so `apk` and Alpine's documentation apply as they are.

The [README](../README.md) has the short version: build, install, update. This
wiki explains the rest.

## Using it

- [Installing](Installing.md): the build container, the installer, the first boot
- [Updating](Updating.md): Alpine's packages, this repository's packages, `.apk-new` files, `doctor`
- [Desktop](Desktop.md): dwl, PipeWire, devices, fonts, the clipboard, screen colour
- [Steam and games](Steam-and-Games.md): the Arch tree, Proton, umu, MangoHud, gamescope, controllers
- [Cloudflare WARP](Cloudflare-WARP.md): WireGuard without the official client
- [Docker, Podman and ROCm](Docker-and-ROCm.md)
- [Troubleshooting](Troubleshooting.md): known messages and their fixes

## How it works

- [Hardware and kernel](Hardware-and-Kernel.md): the machine, what the kernel has and leaves out, and why
- [Boot and init](Boot-and-Init.md): Limine, `rcS`, runit, shutdown, memory
- [Network and firewall](Network-and-Firewall.md): DHCP, IPv6 privacy, nftables, failing closed
- [Recipes](Recipes.md): how a package in `pkgs/` is written and built
- [CI and releases](CI-and-Releases.md): the build, the boot test, the kernel cache, the weekly kernel bump
- [Design](Design.md): the choices behind it all, and what was left out on purpose

## At a glance

| | |
|---|---|
| Base | Alpine Linux edge, musl, apk-tools 3 |
| Init | BusyBox `init`, `rcS`/`rcK` shell scripts, BusyBox's runit tools for services |
| Kernel | Linux LTS (6.18), `tinyconfig` plus a fragment, modules signed and enforced |
| Boot | Limine on UEFI, no initramfs, root mounted by partition name |
| Devices | mdevd, with libudev-zero for programs that want libudev |
| Memory | mimalloc in secure mode for services and logins, zram swap |
| Desktop | dwl (Wayland), PipeWire, seatd |
| Games | Steam and Proton from an Arch Linux tree entered with bubblewrap |
