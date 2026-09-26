# The image and the package snapshot share a date, so rebuilding this file
# installs the same toolchain every time. Move both together.
ARG ARCH_IMAGE=archlinux:base-devel-20260920.0.596911
FROM ${ARCH_IMAGE}
ARG ARCH_SNAPSHOT=2026/09/20

RUN echo "Server = https://archive.archlinux.org/repos/${ARCH_SNAPSHOT}/\$repo/os/\$arch" \
        > /etc/pacman.d/mirrorlist \
    && pacman -Syu --noconfirm --needed \
        base-devel bc cpio curl e2fsprogs git kmod libelf \
        meson ncurses ninja openssl perl xz zlib zstd \
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
