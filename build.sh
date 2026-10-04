#!/bin/sh
# Builds this repository's packages and a BusyLinux root file system from
# Alpine edge, inside the builder container. See the README.
set -eu

ALPINE_MIRROR=${ALPINE_MIRROR:-https://mirror.maeen.sa/alpine}
PACKAGES=${PACKAGES:-}
BASE="alpine-baselayout alpine-keys apk-tools busybox busybox-binsh busybox-suid
      doas e2fsprogs mdev-conf musl-utils amd-ucode nftables runit limine-efi-x86_64
      linux-busylinux@busylinux busylinux-init@busylinux"

PKGS=/usr/local/share/busylinux/pkgs
CACHE=/build/cache OUT=/build/out WORK=/build/work ROOT=/build/rootfs
SRC=$CACHE/sources REPO=$CACHE/packages/x86_64 KEY=$CACHE/keys/busylinux.rsa
LOCAL=/var/lib/busylinux/repo
export VM_SUPPORT=${VM_SUPPORT:-0} MENUCONFIG=${MENUCONFIG:-0}
# shellcheck source=lib.sh
. /usr/local/share/busylinux/lib.sh

# Everything a package is built from; a change rebuilds it.
stamp() {
    { (cd "$PKGS/$1" && find . -type f | LC_ALL=C sort | xargs sha256sum)
      cat /usr/local/share/busylinux/lib.sh /etc/alpine-release
      echo "VM_SUPPORT=$VM_SUPPORT"; } | sha256sum | cut -d' ' -f1
}

mkdir -p "$CACHE/keys/pub" "$CACHE/stamps" "$OUT" "$REPO" "$WORK"
new_key "$CACHE/keys/pub"
cp "$CACHE/keys/pub/busylinux.rsa.pub" /etc/apk/keys/

keep=''
for r in $(recipes); do
    pkgver=$(field "$r" version)-r$(field "$r" release)
    keep="$keep $r-$pkgver.apk"
    s="$pkgver $(stamp "$r")"
    if [ -f "$REPO/$r-$pkgver.apk" ] && [ "$(cat "$CACHE/stamps/$r" 2>/dev/null)" = "$s" ]; then
        log "$r $pkgver: cached"
    else
        build_pkg "$r" "$(field "$r" release)"
        echo "$s" > "$CACHE/stamps/$r"
    fi
done
for f in "$REPO"/*.apk "$SRC"/*; do
    case " $keep $(cat "$PKGS"/*/sha256sums | awk '{print $2}' | tr '\n' ' ') " in
        *" ${f##*/} "*) ;;
        *) rm -f "$f" ;;
    esac
done
index

log "Installing the image from Alpine edge"
rm -rf "$ROOT"
mkdir -p "$ROOT/etc/apk/keys" "$ROOT$LOCAL"
cp /etc/apk/keys/* "$ROOT/etc/apk/keys/"
cp -R "$REPO" "$ROOT$LOCAL/"
repos() {
    printf '%s\n' "v3 @busylinux $1" "$ALPINE_MIRROR/edge/main" \
        "$ALPINE_MIRROR/edge/community" "@testing $ALPINE_MIRROR/edge/testing"
}
repos "${REPO%/*}" > "$WORK/repositories"
# shellcheck disable=SC2086
apk --root "$ROOT" --repositories-file "$WORK/repositories" add --initdb -q $BASE $PACKAGES
repos "$LOCAL" > "$ROOT/etc/apk/repositories"
sed -i 's/^root:[^:]*:/root::/' "$ROOT/etc/shadow"
[ -x "$ROOT/sbin/init" ] || die "the image has no /sbin/init"

log "Initramfs"
I=$WORK/initramfs
rm -rf "$I"
mkdir -p "$I/bin" "$I/dev" "$I/lib" "$I/newroot" "$I/proc" "$I/sys"
mknod -m 600 "$I/dev/console" c 5 1
mknod -m 666 "$I/dev/null" c 1 3
cp "$ROOT/bin/busybox" "$I/bin/"
cp "$ROOT/lib/ld-musl-x86_64.so.1" "$I/lib/"
cp "$PKGS/busylinux-init/files/initramfs-init" "$I/init"
chmod 755 "$I/init"
(cd "$I" && find . | cpio -o -H newc -R 0:0 --quiet) | gzip -9 > "$ROOT/boot/initramfs.cpio.gz"

log "rootfs.tar.gz"
tar -C "$ROOT" -czf "$OUT/rootfs.tar.gz" .
[ -z "${HOST_UID:-}" ] || chown "$HOST_UID:${HOST_GID:-$HOST_UID}" "$OUT/rootfs.tar.gz"
info "$(du -sh "$ROOT" | cut -f1) installed, $(apk --root "$ROOT" list -I | wc -l) packages"
