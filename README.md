# BusyLinux

A musl Linux distribution built on **Alpine Linux edge**, with a kernel
configured from `tinyconfig` for one specific machine and BusyBox init instead
of OpenRC.

Alpine owns the core. musl, BusyBox, apk-tools and every library come straight
from Alpine's repositories, so the ~36,000 packages in edge install and run
unmodified. This project owns three things:

- **the kernel** built from `tinyconfig` plus a hardware fragment, not defconfig;
- **the init layer** BusyBox init, one conditional `rcS`, and the `/etc` files
  that go with it;
- **the image** disk image, UEFI ISO, initramfs, an installer, and a signed apk repository.
- **the boot loader** Limine 12.9 (Alpine's), UEFI only.

## Building

The build runs entirely inside an Arch Linux container; nothing is compiled on the host.

```sh
apk add docker qemu-system-x86_64
rc-service docker start
addgroup "$USER" docker          # log out and back in
git clone https://github.com/LBarrys/busylinux.git
cd busylinux
docker build -t busylinux-builder .
mkdir -p out
docker run --rm -it -v "$PWD/out:/build/out" -v busylinux-cache:/build/cache -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" busylinux-builder
```

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
qemu-system-x86_64 -m 2G -nographic -kernel out/vmlinuz -initrd out/initramfs.cpio.gz -append "root=LABEL=BUSYLINUX_ROOT console=ttyS0" -drive file=out/disk.img,format=raw,if=virtio -nic user,model=virtio-net-pci
```
Log in as `root` with no password.

## Installing on real hardware

`install.sh` does the whole thing. Run it as root from an Alpine live USB — any
Linux with the tools below will do, but Alpine is musl like the target, so the
chroot at the end works without surprises.

```sh
apk add sgdisk dosfstools e2fsprogs parted efibootmgr
./install.sh --disk /dev/nvme0n1 --user alice
```

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

## License

The build system and the files under `pkgs/busylinux-init/` are MIT. The Linux
kernel built by `pkgs/linux-busylinux/` is GPL-2.0-only and is not distributed
here — only the recipe that fetches and configures it. Everything else in a
built image comes from Alpine Linux under its own licenses.

## Acknowledgements

- [Alpine Linux](https://alpinelinux.org/) musl, BusyBox, apk-tools and
  everything above the kernel.
- [Limine](https://limine-bootloader.org/) the boot loader.
- [BusyBox](https://busybox.net/) init, the shell and most of the userland.
- Substantial parts of the build system, installer and documentation were
  written with [Claude](https://claude.ai) (Anthropic).
