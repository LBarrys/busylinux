# Move the tag and digest together.
ARG ALPINE_IMAGE=alpine:3.24.2@sha256:294b683cb724975bec92580e1e685676bd4b50bda910ddb8c51d4cabeaec77e6
FROM ${ALPINE_IMAGE}

RUN apk add --no-cache \
        bash bc bison build-base coreutils cpio curl diffutils e2fsprogs \
        elfutils-dev findutils flex gawk git grep gzip kmod linux-headers \
        ncurses-dev openssl openssl-dev perl sed tar xz zlib-dev zstd

COPY build.sh /usr/local/bin/build.sh
COPY pkgs     /usr/local/share/busylinux/pkgs
RUN chmod 755 /usr/local/bin/build.sh \
              /usr/local/share/busylinux/pkgs/*/build \
              /usr/local/share/busylinux/pkgs/busylinux-init/files/rcS \
              /usr/local/share/busylinux/pkgs/busylinux-init/files/rcK \
              /usr/local/share/busylinux/pkgs/busylinux-init/files/initramfs-init

WORKDIR /build
CMD ["/usr/local/bin/build.sh"]
