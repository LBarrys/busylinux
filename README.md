# BusyLinux

A musl system built on **Alpine Linux edge**, with a kernel configured from
`tinyconfig` for one specific machine and BusyBox init instead of OpenRC.

`ID=busylinux`, `ID_LIKE=alpine`, hostname `busylinux`, root filesystem label
`BUSYLINUX_ROOT`.

Alpine owns the core — musl, BusyBox, apk-tools and every library come straight
from their repositories, so the ~15,000 packages in edge install without any
ABI games. This project owns three things:

- **the kernel**, built from `tinyconfig` with a hardware fragment, not defconfig;
- **the init layer**, BusyBox init with one `rcS` that starts only what is
  installed, plus `/etc/inittab`, `/etc/issue`, `/etc/motd` and
  `/etc/os-release`;
- **the image**: disk image, ISO, initramfs, an installer, and a signed
  repository for the above.

| | |
|---|---|
| Kernel | 6.18.52, **3.9 MB**, 1,289 options enabled |
| Base image | **43 MB**, 21 packages (8.4 MB of it the bundled repository) |
| libc | musl 1.2.6 (Alpine's) |
| Package manager | apk-tools 3.0.8 (Alpine's), v2 and v3 repositories |
| Boot loader | Limine 12.9 (Alpine's), UEFI only |

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

Two things on this board are USB devices that do not look like USB devices.
The ALC4080 is a **USB** codec on an internal port, so `CONFIG_SND_USB_AUDIO`
is what makes sound work — HDA is built only for the display audio on the GPU.
The MT7922 is likewise split: the Wi-Fi half is PCIe and binds `mt7921e`, while
its Bluetooth half is a USB device, so it needs `btusb` with
`CONFIG_BT_HCIBTUSB_MTK=y` (which pulls in `btmtk`). Both halves want firmware:
`apk add linux-firmware-mediatek`.

## Build

```sh
doas apk add docker qemu-system-x86_64
doas rc-service docker start
doas addgroup "$USER" docker          # log out and back in

tar -xzf busylinux.tar.gz && cd busylinux
docker build -t busylinux-builder .
mkdir -p out
docker run --rm -it \
  -v "$PWD/out:/build/out" -v busylinux-cache:/build/cache \
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
  busylinux-builder
```

**Keep the `busylinux-cache` volume**: it holds the kernel source, the built packages
and this repository's private signing key.

| Variable | Effect |
|----------|--------|
| `PACKAGES="..."` | extra Alpine packages to bake into the image |
| `BASE_PACKAGES="..."` | replace the default package set |
| `ALPINE_BRANCH=v3.24` | build against a stable release instead of edge |
| `ALPINE_MIRROR=...` | use a closer mirror |
| `REPO_URL=https://…` | an extra network URL for *this* repository |
| `LOCAL_REPO=0` | do not copy this repository into the image |
| `VM_SUPPORT=0` | drop virtio/bochs: a kernel that only knows your hardware |
| `MENUCONFIG=1` | open menuconfig after the fragments are merged |
| `REBUILD=1` | rebuild packages even when a cached `.apk` exists |
| `JOBS=N`, `IMAGE_SIZE=16G` | as before |

## Run

```sh
qemu-system-x86_64 -m 2G -nographic \
  -kernel out/vmlinuz -initrd out/initramfs.cpio.gz \
  -append "root=LABEL=BUSYLINUX_ROOT console=ttyS0" \
  -drive file=out/disk.img,format=raw,if=virtio \
  -nic user,model=virtio-net-pci
```

Log in as `root` with no password. `out/busylinux.iso` is a UEFI boot medium
for the same kernel — 9 MB, Limine and nothing else — useful for booting or
rescuing a machine whose ESP has gone wrong. It is not a live system: it looks
for a disk labelled `BUSYLINUX_ROOT`, and drops to the rescue shell otherwise.

## Installing on real hardware

`install.sh` does the whole thing. Run it from an Alpine live USB (any Linux
with the tools below will do, but Alpine is musl like the target, so the chroot
at the end works without surprises):

```sh
apk add sgdisk dosfstools e2fsprogs parted efibootmgr

./install.sh --disk /dev/nvme0n1 --user alice
```

There is no boot loader in that list. Limine on UEFI is an EFI application and
a text file, and its `BOOTX64.EFI` comes out of the root filesystem that was
just unpacked — so the machine you install *from* needs nothing but the
partitioning tools.

It asks you to type the disk name before it touches anything, then writes:

| | | |
|---|---|---|
| 1 | 1 GiB | EFI system, FAT32, **mounted at `/boot`** |
| 2 | rest | root, ext4, `LABEL=BUSYLINUX_ROOT` |

and unpacks `rootfs.tar.gz`, copies the initramfs and microcode, writes
`/etc/fstab`, drops Limine and its config into `\EFI\BOOT`, and adds a UEFI
boot entry with `efibootmgr` if it can. `\EFI\BOOT\BOOTX64.EFI` is the
removable path every firmware falls back to, so the board still boots if its
NVRAM is ever cleared, and re-running the installer replaces its own old entry
instead of stacking duplicates.

The ESP *is* `/boot`, which is the point: `apk add linux-busylinux` writes the
new kernel straight to the partition the firmware reads, and `limine.conf`
already names it. The initramfs contains only BusyBox and holds no modules, so
it never needs regenerating.

| Option | |
|---|---|
| `--user NAME` | create a user in `video`, `input`, `audio`, `seat` and set a password |
| `--esp-size 2G` | bigger ESP if you plan to keep several kernels |
| `--root-size 200G` | leave the rest of the disk unpartitioned |
| `--hostname NAME` | default `busylinux` |
| `--no-nvram` | removable path only, do not touch the boot menu |
| `--yes` | skip the confirmation |

### In the firmware

- **Secure Boot off.** This kernel is not signed and never will be here.
- **CSM off**, boot in UEFI mode. There is no BIOS path at all.
- EXPO stays on. Memory training is firmware-side; the kernel needs nothing.

### After the first boot

```sh
apk update
apk add linux-firmware-amdgpu linux-firmware-mediatek
```

Without those two the GPU falls back to a bare framebuffer and the MT7922 does
not associate. Then the desktop, below.

### If you would rather do it by hand

Nothing here is magic — partition and format, then:

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
has to be **first**: the kernel reads it before it does anything else with the
CPU.

Limine looks for its config beside its own EFI application before anywhere
else, so `\EFI\BOOT\limine.conf` cannot be shadowed by a stray config on
another partition.

One thing to watch: the root filesystem is found by label, so if another disk
in the machine also carries `BUSYLINUX_ROOT` the initramfs picks whichever it
finds first. Relabel one of them (`e2label`) or use `root=UUID=...`.

## BusyBox: vi, ash and the line editor

BusyBox arrives as Alpine's binary package, so there is no `menuconfig` step in
this build — the applet set and every `CONFIG_FEATURE_*` are fixed by
[`main/busybox/busyboxconfig`](https://git.alpinelinux.org/aports/tree/main/busybox/busyboxconfig)
in aports. That config is close to maximal for the things worth having:

| | Already on in Alpine's build |
|---|---|
| vi | `COLON`, `COLON_EXPAND`, `YANKMARK`, `SEARCH`, `DOT_CMD`, `READONLY`, `SET`, `SETOPTS`, `WIN_RESIZE`, `ASK_TERMINAL`, `USE_SIGNALS`, `UNDO` + `UNDO_QUEUE=256`, `8BIT`, `MAX_LEN=4096` |
| ash | `BASH_COMPAT`, `BASH_SOURCE_CURDIR`, `BASH_NOT_FOUND_HOOK`, `JOB_CONTROL`, `ALIAS`, `RANDOM_SUPPORT`, `EXPAND_PRMT`, `IDLE_TIMEOUT`, `ECHO`, `PRINTF`, `TEST`, `HELP`, `GETOPTS`, `CMDCMD`, `VERSION_VAR`, `SH_MATH_64`, `SH_HISTFILESIZE` |
| line editor | `EDITING`, `EDITING_VI`, `HISTORY=2000`, `SAVEHISTORY`, `FANCY_PROMPT`, `WINCH`, `TAB_COMPLETION`, `USERNAME_COMPLETION`, `REVERSE_SEARCH`, `LOCALE_SUPPORT`, `UNICODE_SUPPORT` with combining and wide characters |

Exactly three things are off that you would notice:

| Off | What you lose |
|---|---|
| `FEATURE_VI_REGEX_SEARCH` | `/pattern` and `:s///` match literal text, not POSIX regex |
| `FEATURE_VI_VERBOSE_STATUS` | no "5 lines deleted" style messages on the status line |
| `FEATURE_EDITING_SAVE_ON_EXIT` | history is appended per command instead of rewritten at exit — cosmetic |

Two of those cost nothing to work around, and neither needs a rebuild:

```sh
set -o vi                    # vi keybindings in ash (EDITING_VI is compiled in)
apk add vim                  # regex, syntax highlighting, a real undo tree
apk add bash bash-completion
```

If you want the missing symbols anyway, the only honest way is to build
BusyBox from source and shadow Alpine's package — which undoes "Alpine owns the
core". It can be done safely as a **static** musl binary (Arch's `musl` package
gives you `musl-gcc`, and a static BusyBox has no ABI surface at all), packaged
as `busybox-busylinux` with `provides=busybox` and `replaces=busybox`. It is not
in this tree; say the word and it goes in as an opt-in recipe.

## Installing the desktop

The base is deliberately bare. Everything below is one `apk add` away, and all
of it exists in edge today. **Do not drop `eudev` from this list** — see
"The two that bite" below for why.

```sh
apk add \
    linux-firmware-amdgpu linux-firmware-mediatek \
    mesa-dri-gallium mesa-va-gallium mesa-vulkan-ati \
    vulkan-loader eudev dbus rtkit seatd mkrundir \
    iwd bluez wireless-regdb \
    pipewire pipewire-alsa pipewire-pulse wireplumber \
    sway swaybg swayidle swaylock foot waybar tofi xwayland \
    xdg-desktop-portal xdg-desktop-portal-gtk \
    papirus-icon-theme breeze-cursors wl-clipboard grim slurp \
    font-noto font-noto-extra font-noto-arabic font-noto-cjk \
    font-noto-cjk-extra font-noto-emoji font-noto-symbols \
    firefox flatpak 7zip

# these four live in testing, which is tagged so it is never used by accident
apk add cliphist@testing gammastep@testing wiremix@testing unrar-free@testing
```

That comes to 318 packages and about 1.5 GB installed. For niri instead of
sway, swap `sway swaybg` for `niri xwayland-satellite`.

`iwd` handles Wi-Fi association (`iwctl station wlan0 connect SSID`) and BlueZ
is started by `rcS`, so `bluetoothctl` works once it is installed.

### The two that bite

**`eudev` is not optional.** `apk add` will pull in `eudev-libs` on its own,
because libinput links against `libudev.so.1` — but that is the *library*, not
the daemon. Without `eudev` there is no `udevd`, and `libudev`'s monitor
listens on the netlink group that only `udevd` broadcasts on. Enumeration at
startup still works off sysfs, so a compositor comes up; nothing plugged in
afterwards is ever noticed. A monitor or keyboard connected after login simply
does not appear. `rcS` also falls back to `mdev -d` when `udevd` is absent.

**The user must be in `seat`.** `rcS` runs `seatd -g seat`, and the socket is
`srwxrwx--- root:seat`. A compositor that cannot open it dies immediately:

```
thread 'main' panicked at src/main.rs:186:6:
called `Result::unwrap()` on an `Err` value: error initializing the TTY backend
Caused by:
    0: Error creating a session.
    1: Failed to open session: Function not implemented (os error 38)
```

`install.sh --user NAME` adds all four. Making a user by hand:

```sh
adduser alice
for g in video input seat audio; do addgroup alice $g; done
```

Then log in **on tty1**, not over serial or ssh: the TTY backend wants a real
virtual terminal to take over.

### Starting a session

There is no display manager and no OpenRC. `rcS` has already started `seatd`,
the system D-Bus and `udevd`; the rest belongs to your session. Log in on tty1
and run:

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

`XDG_RUNTIME_DIR` is already set for you by `/etc/profile.d/busylinux.sh`.

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
| `bluetoothd` | if bluez is installed (and dbus is running) |
| `udhcpc` on every Ethernet/Wi-Fi interface | always |

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
**`mdev-conf`** package is therefore part of the base. It restores the usual
modes and does rather more: `@modprobe -q -b "$MODALIAS"` loads a driver when
the device appears, and `/lib/mdev/persistent-storage` builds `/dev/disk/by-*`.
Install `eudev` and `rcS` uses `udevd` instead, which brings its own rules.

## The kernel

`pkgs/linux-busylinux/files/busylinux.config` is a Kconfig fragment merged onto
`make tinyconfig`. Every line is something the machine above needs; nothing
else is on. Adding hardware means adding a line:

```sh
echo 'CONFIG_BTRFS_FS=m' >> pkgs/linux-busylinux/files/busylinux.config
# bump release= in pkgs/linux-busylinux/meta, then rebuild
```

`pkgs/linux-busylinux/files/vm.config` adds virtio and bochs so the same kernel boots
under QEMU for testing. `VM_SUPPORT=0` drops it.

### tinyconfig hides things, so the build checks

Starting from nothing means an option whose dependency is missing is dropped
**silently**. The recipe therefore asserts ~40 symbols survived `olddefconfig`
and fails the build otherwise. Three that were found this way, all
EXPERT-gated and all off by default in `tinyconfig`:

| Symbol | Without it |
|---|---|
| `CONFIG_TTY` | no virtual terminals *and* no serial console — a silent machine |
| `CONFIG_FILE_LOCKING` | `apk` cannot lock its database: "Function not implemented" |
| `CONFIG_MEMFD_CREATE` | Wayland buffers, PipeWire and Mesa fail in obscure ways |

If you add hardware and the build stops with `config lost: FOO`, the option's
dependency is missing — add that too rather than deleting the check.

## Repositories

`/etc/apk/repositories` in the image:

```
v3 /var/lib/busylinux/repo
https://dl-cdn.alpinelinux.org/alpine/edge/main
https://dl-cdn.alpinelinux.org/alpine/edge/community
@testing https://dl-cdn.alpinelinux.org/alpine/edge/testing
```

`testing` is **tagged**: it is never used unless you name it, so a stray
testing build cannot replace something from community. `apk add wiremix@testing`
opts in for that one package.

### Why the image carries its own repository

The kernel and `busylinux-init` do not exist in any Alpine repository, so
without somewhere to find them apk treats them as orphans and says so on every
interactive run:

```
NOTE: Consider running apk upgrade with --prune and/or --available.
The following packages are no longer available from a repository:
  busylinux-init linux-busylinux
```

**Do not take that advice.** `apk upgrade --prune` purges exactly those two
packages — the kernel and `/sbin/init` — and the machine will not boot again.

The build therefore copies the current release of each of its own packages into
`/var/lib/busylinux/repo` inside the image (about 8.4 MB, which is most of the
difference between a 35 MB and a 42 MB base) and puts that path first in
`/etc/apk/repositories`. apk reads a plain filesystem path as a repository, so
nothing has to be served. `LOCAL_REPO=0` turns this off.

To fix a machine built before this existed, without rebuilding:

```sh
doas mkdir -p /var/lib/busylinux/repo
doas cp -r out/repo/x86_64 /var/lib/busylinux/repo/
doas cp out/repo/busylinux.rsa.pub /etc/apk/keys/     # if it is not there yet
doas sed -i '\|^#v3 |c\v3 /var/lib/busylinux/repo' /etc/apk/repositories
```

### Over the network instead

The same repository is published to `out/repo` on every build, signed with a
key generated on the first build and kept in the cache volume. Serve it and set
`REPO_URL=` to add a second line, so the machine can pull kernel updates:

```sh
cd out/repo && python3 -m http.server 8080
```

Alpine's repositories are v2 and this one is v3; apk-tools 3 reads both, which
is why they can sit in the same file.

## Adding your own packages

`pkgs/<name>/` holds `meta`, `sources`, `sha256sums`, `build`, and optionally
`files/`, `patches/` and `split/`. `meta` is a shell fragment with `version`,
`release`, `desc`, `depends`, `replaces`, `options`. The build script receives
the destination directory as `$1`:

```sh
#!/bin/sh -e
make
make DESTDIR="$1" install
```

Recipes are built with the **container's** toolchain, which is right for the
kernel and for anything freestanding. There is no musl sysroot here, so a
package that links against libc should come from Alpine instead — that is the
whole point of letting Alpine own the core.

`busylinux-init` shows how to take ownership of a file Alpine already ships: it sets
`replaces="alpine-baselayout-data"` so its `/etc/inittab` can replace theirs.

```sh
recipes="-v $PWD/pkgs:/usr/local/share/busylinux/pkgs"
docker run --rm $recipes -v busylinux-cache:/build/cache busylinux-builder build.sh --checksum mypkg
docker run --rm -it $recipes -v "$PWD/out:/build/out" -v busylinux-cache:/build/cache busylinux-builder
```

## Limits

- **The hardware is untested.** This was built and booted under QEMU. The
  amdgpu, r8169, mt7921e, btusb and ALC4080 paths cannot be exercised here —
  they are configured from the board's specifications. Boot it in a VM first,
  then from a USB stick before installing. `lspci -k` and `lsusb -t` on the
  running machine will confirm which drivers bind.
- **Wireless needs firmware and userspace.** The kernel side is built in, but
  `mt7921e` will not associate without `linux-firmware-mediatek`, and there is
  no `wpa_supplicant`, `iw` or `bluetoothd` in the base image. `rcS` starts
  BlueZ if you install it; Wi-Fi association is up to you (`iwd` or
  `wpa_supplicant`).
- **No display manager, no OpenRC.** Session services are your responsibility,
  as described above.
- **Flatpak needs its own runtime.** It is glibc-based and self-contained, so
  it works on musl — that is the usual way to run software Alpine does not
  package.
