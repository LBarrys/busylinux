# BusyLinux

Alpine Linux edge with BusyBox init, runit and a `tinyconfig` kernel, made for
one machine (Ryzen 7 7800X3D, Radeon RX 7900 GRE). Everything but `pkgs/` comes
straight from Alpine.

## Build

```sh
docker build -t busylinux-builder .
docker run --rm -v "$PWD/out:/build/out" -v busylinux-cache:/build/cache \
  -e HOST_UID="$(id -u)" busylinux-builder
```

Keep a copy of the signing key, `keys/busylinux.rsa` in the `busylinux-cache` volume.

## Install

From an Alpine live USB, as root, with Secure Boot off. **This erases the disk.**

```sh
apk add sgdisk dosfstools e2fsprogs efibootmgr tzdata kbd-bkeymaps
tools/install.sh --disk /dev/nvme0n1 --user alice --timezone Europe/Berlin --keymap de/de-latin1
```

## Update

```sh
doas apk upgrade                   # Alpine's packages
git pull && doas tools/update.sh --all   # this repository's packages
doas tools/doctor                  # anything that needs you
```

## More

Everything else (the desktop, Steam, WARP, how it all works) is in the
[wiki](wiki/Home.md).

## License

MIT for the build system and `pkgs/busylinux-init/`; GPL-2.0-only for the
kernel. Alpine's packages keep their own licenses.
