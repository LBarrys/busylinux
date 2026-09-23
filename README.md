# BusyLinux

A musl Linux distribution built on **Alpine Linux edge**, with a kernel
configured from `tinyconfig` for one specific machine and BusyBox init instead
of OpenRC.

Alpine owns the core. musl, BusyBox, apk-tools and every library come straight
from Alpine's repositories, so the ~36,000 packages in edge install and run
unmodified. This project owns three things:

- **the kernel** — built from `tinyconfig` plus a hardware fragment, not defconfig;
- **the init layer** — BusyBox init, one conditional `rcS`, and the `/etc` files
  that go with it;
- **the image** — disk image, UEFI ISO, initramfs, an installer, and a signed
  apk repository for the two packages above.
- **the boot loader** ... Limine 12.9 (Alpine's), UEFI only 

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
