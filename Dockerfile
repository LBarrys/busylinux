FROM archlinux:latest

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
