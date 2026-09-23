#!/bin/sh
set -eu

usage() {
    cat <<'EOT'
Build, package and install a BusyLinux kernel on the machine itself.

    kernel-update.sh [options]

Runs as root from a checkout of this repository. It installs the toolchain if
needed, fetches and configures the kernel with this project's fragment, builds
it, wraps it in a signed .apk, adds it to the local repository and installs it.

Options:
    --version X.Y.Z     kernel version (default: pkgs/linux-busylinux/meta).
                        The kernel.org directory follows the major number, so
                        7.2.7 is fetched from v7.x and 6.18.53 from v6.x.
    --sha256 SUM        expected checksum, for a version this tree has none for
    --config-only       configure and check the symbols, then stop
    --release N         apk release number (default: installed release + 1)
    --jobs N            parallel make jobs (default: nproc)
    --workdir DIR       build directory (default: /var/tmp/busylinux-kernel)
    --checkout DIR      where this repository is, if not beside the script
    --key FILE          signing key (default: /root/keys/local.rsa, created)
    --repo DIR          local repository (default: /var/lib/busylinux/repo/x86_64)
    --menuconfig        open menuconfig after the fragments are merged
    --no-vm             leave out virtio and bochs
    --no-fallback       do not keep the running kernel as vmlinuz-previous
    --no-install        build and package only
    --keep-source       reuse an existing build tree instead of extracting
    --yes               do not ask for confirmation
EOT
}

VERSION='' RELEASE='' JOBS='' WORK=/var/tmp/busylinux-kernel
KEY=/root/keys/local.rsa REPO=/var/lib/busylinux/repo/x86_64
VM=1 MENUCONFIG=0 FALLBACK=1 INSTALL=1 KEEP=0 ASSUME_YES=0 CONFIG_ONLY=0
SHA256='' CHECKOUT='' NAME=linux-busylinux
MIRROR=https://cdn.kernel.org/pub/linux/kernel

die()  { printf '\033[1;31merror: %s\033[0m\n' "$*" >&2; exit 1; }
log()  { printf '\n\033[1;32m==> %s\033[0m\n' "$*"; }
info() { printf '\033[1;34m  > %s\033[0m\n' "$*"; }

while [ $# -gt 0 ]; do
    case $1 in
        --version)     VERSION=$2; shift 2 ;;
        --release)     RELEASE=$2; shift 2 ;;
        --jobs)        JOBS=$2; shift 2 ;;
        --workdir)     WORK=$2; shift 2 ;;
        --checkout)    CHECKOUT=$2; shift 2 ;;
        --key)         KEY=$2; shift 2 ;;
        --repo)        REPO=$2; shift 2 ;;
        --sha256)      SHA256=$2; shift 2 ;;
        --config-only) CONFIG_ONLY=1; shift ;;
        --menuconfig)  MENUCONFIG=1; shift ;;
        --no-vm)       VM=0; shift ;;
        --no-fallback) FALLBACK=0; shift ;;
        --no-install)  INSTALL=0; shift ;;
        --keep-source) KEEP=1; shift ;;
        --yes|-y)      ASSUME_YES=1; shift ;;
        -h|--help)     usage; exit 0 ;;
        *)             die "unknown option: $1" ;;
    esac
done

[ "$(id -u)" = 0 ] || die "run this as root"

HERE=$(cd "$(dirname "$0")" && pwd -P)
NL='
'
RECIPE='' TRIED=''
for d in "$CHECKOUT" "$HERE" "$PWD" /usr/local/share/busylinux /root/busylinux; do
    [ -n "$d" ] || continue
    case ${d%/} in '') p=/pkgs/$NAME ;; *) p=${d%/}/pkgs/$NAME ;; esac
    case $NL$TRIED in *"$NL$p$NL"*) continue ;; esac
    TRIED=$TRIED$p$NL
    if [ -f "$p/build" ]; then RECIPE=$p; break; fi
done
if [ -z "$RECIPE" ]; then
    printf '\033[1;31merror: could not find pkgs/%s/build\033[0m\n' "$NAME" >&2
    printf 'Looked in:\n' >&2
    printf '%s' "$TRIED" | sed 's/^/    /' >&2
    cat >&2 <<EOT

This script drives the recipe in the repository rather than repeating it, so it
needs the checkout. Run it from there:

    apk add git
    git clone https://github.com/LBarrys/busylinux.git
    cd busylinux
    ./kernel-update.sh

or point at an existing checkout with --checkout DIR.
EOT
    exit 1
fi

if [ -z "$VERSION" ]; then
    VERSION=$(sed -n 's/^version=//p' "$RECIPE/meta")
fi
[ -n "$VERSION" ] || die "could not determine the kernel version"

INSTALLED=$(apk list --installed 2>/dev/null |
            sed -n "s/^$NAME-\([0-9][^ ]*\) .*/\1/p" | head -1)
if [ -z "$RELEASE" ]; then
    case $INSTALLED in
        "$VERSION"-r*) RELEASE=$(( ${INSTALLED##*-r} + 1 )) ;;
        *)             RELEASE=0 ;;
    esac
fi
PKGVER=$VERSION-r$RELEASE
TARBALL=linux-$VERSION.tar.xz
SERIES=v${VERSION%%.*}.x
SRCDIR=$WORK/linux-$VERSION
PKGDIR=$WORK/pkg-$PKGVER
: "${JOBS:=$(nproc)}"

log "BusyLinux kernel $PKGVER"
info "installed now: ${INSTALLED:-none}"
info "build tree:    $SRCDIR"
info "jobs:          $JOBS"
if [ "$VM" = 1 ]; then info "virtio/bochs:  included"; else info "virtio/bochs:  left out"; fi
if [ "$ASSUME_YES" != 1 ]; then
    printf 'Continue [y/N]? '
    read -r answer
    case $answer in y|Y|yes) ;; *) die "not confirmed" ;; esac
fi

log "Checking the toolchain"
TOOLS="build-base bash bc bison flex perl openssl openssl-dev elfutils-dev
       linux-headers diffutils findutils xz gzip cpio zstd"
if [ "$MENUCONFIG" = 1 ]; then TOOLS="$TOOLS ncurses-dev"; fi
missing=''
for t in gcc make bc bison flex perl openssl xz cpio; do
    command -v "$t" > /dev/null 2>&1 || missing="$missing $t"
done
if [ -n "$missing" ]; then
    info "missing:$missing"
    # shellcheck disable=SC2086
    apk add $TOOLS
else
    info "already present"
fi

mkdir -p "$WORK"
avail=$(df -P "$WORK" 2>/dev/null | awk 'NR==2 {printf "%d", $4/1048576}')
case ${avail:-x} in
    ''|*[!0-9]*) info "could not measure free space in $WORK; about 3G is needed" ;;
    *) [ "$avail" -ge 4 ] ||
         die "only ${avail}G free in $WORK; a build tree needs about 3G" ;;
esac

log "Fetching linux-$VERSION"
if [ -s "$WORK/$TARBALL" ]; then
    info "already downloaded"
else
    wget -q -O "$WORK/$TARBALL.part" "$MIRROR/$SERIES/$TARBALL" ||
        die "could not download $MIRROR/$SERIES/$TARBALL"
    mv "$WORK/$TARBALL.part" "$WORK/$TARBALL"
fi
want=$SHA256
if [ -z "$want" ]; then
    want=$(sed -n "s/  $TARBALL\$//p" "$RECIPE/sha256sums" | head -1)
fi
if [ -n "$want" ]; then
    got=$(sha256sum "$WORK/$TARBALL" | cut -d' ' -f1)
    [ "$got" = "$want" ] || die "$TARBALL checksum $got, expected $want"
    info "checksum ok"
else
    info "this tree has no checksum for $TARBALL, so only TLS vouches for it."
    info "Pass --sha256 with the value from $MIRROR/$SERIES/sha256sums.asc"
fi

if [ "$KEEP" = 1 ] && [ -d "$SRCDIR" ]; then
    log "Reusing the existing build tree"
else
    log "Extracting"
    rm -rf "$SRCDIR"
    tar -xf "$WORK/$TARBALL" -C "$WORK"
fi
cp "$RECIPE"/files/*.config "$SRCDIR/"

log "Configuring and building"
rm -rf "$PKGDIR"
mkdir -p "$PKGDIR"
MAKEFLAGS="-j$JOBS"
export MAKEFLAGS VM_SUPPORT="$VM" MENUCONFIG
if [ "$CONFIG_ONLY" = 1 ]; then
    ( cd "$SRCDIR" && CONFIG_ONLY=1 sh -e "$RECIPE/build" "$PKGDIR" "$VERSION" )
    log "Done (configuration only)"
    exit 0
fi
( cd "$SRCDIR" && sh -e "$RECIPE/build" "$PKGDIR" "$VERSION" )

if [ "$FALLBACK" = 1 ] && [ -f /boot/vmlinuz-busylinux ]; then
    log "Keeping the running kernel as a fallback"
    cp /boot/vmlinuz-busylinux /boot/vmlinuz-previous
    conf=/boot/EFI/BOOT/limine.conf
    if [ -f "$conf" ] && ! grep -q 'vmlinuz-previous' "$conf"; then
        ucode=''
        if [ -f /boot/amd-ucode.img ]; then
            ucode='    module_path: boot():/amd-ucode.img
'
        fi
        printf '\n/BusyLinux (previous kernel)\n    protocol: linux\n    path: boot():/vmlinuz-previous\n    cmdline: root=LABEL=BUSYLINUX_ROOT rw\n%s    module_path: boot():/initramfs.cpio.gz\n' "$ucode" >> "$conf"
        info "added a 'previous kernel' entry to limine.conf"
    fi
    info "/boot/vmlinuz-previous saved"
    case $INSTALLED in
        "$VERSION"-r*)
            info "same kernel version: the fallback will run with the NEW modules,"
            info "which MODULE_SIG_FORCE refuses. It reaches a console, not a desktop." ;;
    esac
fi

log "Packaging $NAME-$PKGVER"
if [ ! -f "$KEY" ]; then
    mkdir -p "$(dirname "$KEY")"
    openssl genrsa -out "$KEY" 4096 2>/dev/null
    chmod 600 "$KEY"
    openssl rsa -in "$KEY" -pubout -out "/etc/apk/keys/$(basename "$KEY").pub" 2>/dev/null
    info "generated $KEY and trusted its public half"
fi
mkdir -p "$REPO"
apk mkpkg --files "$PKGDIR" \
    --info "name:$NAME" --info "version:$PKGVER" \
    --info "description:Linux kernel built on this machine" \
    --info arch:x86_64 --info license:GPL-2.0-only --info "origin:$NAME" \
    --sign-key "$KEY" --output "$REPO/$NAME-$PKGVER.apk"
apk mkndx --output "$REPO/Packages.adb" --sign-key "$KEY" "$REPO"/*.apk > /dev/null
info "$(du -h "$REPO/$NAME-$PKGVER.apk" | cut -f1) in $REPO"

if [ "$INSTALL" != 1 ]; then
    log "Done (not installed)"
    info "install it with: apk update && apk upgrade $NAME"
    exit 0
fi

log "Installing"
apk update > /dev/null
apk upgrade "$NAME"

log "Done"
cat <<EOF

  $NAME-$PKGVER installed
    $(ls -lh /boot/vmlinuz-busylinux | awk '{print $5}')  /boot/vmlinuz-busylinux
    $(find /lib/modules -name '*.ko' | wc -l) modules in /lib/modules

  Reboot to run it. The build tree is still in $SRCDIR;
  remove it, or keep it and pass --keep-source next time.

EOF
