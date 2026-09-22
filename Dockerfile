# Builder image for BusyLinux: a musl system on Alpine edge with a kernel
# built from tinyconfig. Everything is compiled inside this container.
FROM archlinux:latest

# base-devel bc cpio kmod libelf perl xz : the Linux kernel
# meson ninja zlib zstd openssl          : apk-tools, built here to make and
#                                          sign this project's own packages
# e2fsprogs                              : the disk image (mke2fs -d)
# libisoburn (xorriso)                   : the UEFI ISO; the boot loader is
#                                          Limine, which is only a file to copy
# ncurses                                : optional 'make menuconfig'
# curl git                               : sources
RUN pacman -Syu --noconfirm --needed \
        base-devel bc cpio curl e2fsprogs git kmod libelf \
        libisoburn meson ncurses ninja openssl perl xz zlib zstd \
    && rm -rf /var/cache/pacman/pkg/*

COPY build.sh /usr/local/bin/build.sh
COPY pkgs     /usr/local/share/busylinux/pkgs
RUN chmod 755 /usr/local/bin/build.sh \
              /usr/local/share/busylinux/pkgs/*/build \
              /usr/local/share/busylinux/pkgs/busylinux-init/files/rcS \
              /usr/local/share/busylinux/pkgs/busylinux-init/files/rcK \
              /usr/local/share/busylinux/pkgs/busylinux-init/files/initramfs-init

WORKDIR /build
CMD ["/usr/local/bin/build.sh"]
