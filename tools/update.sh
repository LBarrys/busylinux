#!/bin/sh
set -eu

usage() {
    cat <<'EOT'
Builds this repository's packages on the machine itself and installs them.

    update.sh [--yes] recipe...     the named recipes, e.g. libudev-zero or linux-busylinux
    update.sh [--yes] --all         every recipe
    update.sh                       list the recipes

Run as root from a checkout. Recipes build in a separate Alpine root under
/var/tmp/busylinux-update, so build tools never touch the system. Packages are signed with /root/keys/local.rsa (created on
first use) and pinned to this repository: apk add NAME@busylinux. Changed /etc
files are kept; the new versions land beside them as .apk-new.
EOT
}

# The build root has no mimalloc, and musl stops at a missing preload.
unset LD_PRELOAD
TOOLS=$(cd "$(dirname "$0")" && pwd -P)
PKGS=${TOOLS%/*}/pkgs
BR=/var/tmp/busylinux-update/root
REPO=/var/lib/busylinux/repo/x86_64 KEY=/root/keys/local.rsa
# shellcheck source=tools/lib.sh
. "$TOOLS/lib.sh"

YES=0 ALL=0
while [ $# -gt 0 ]; do
    case $1 in
        --yes|-y)  YES=1; shift ;;
        --all)     ALL=1; shift ;;
        -h|--help) usage; exit 0 ;;
        -*)        die "unknown option: $1" ;;
        *)         break ;;
    esac
done

installed() {
    apk list --installed "$1" 2>/dev/null | sed -n "s/^$1-\([0-9][^ ]*\) .*/\1/p"
}

if [ "$ALL" = 1 ]; then
    # shellcheck disable=SC2046
    set -- $(recipes)
elif [ $# -eq 0 ]; then
    for r in $(recipes); do
        info "$r $(field "$r" version)-r$(field "$r" release), installed: $(installed "$r")"
    done
    exit 0
fi
[ "$(id -u)" = 0 ] || die "run this as root"
for r in "$@"; do [ -f "$PKGS/$r/meta" ] || die "no recipe pkgs/$r"; done

log "Building: $*"
if [ "$YES" != 1 ]; then
    printf 'Continue [y/N]? '
    read -r answer
    case $answer in y|Y|yes) ;; *) die "not confirmed" ;; esac
fi
trap 'apk del -q .busylinux-build 2>/dev/null || :; umount "$BR/proc" "$BR/dev" 2>/dev/null || :' EXIT

[ -f "$KEY" ] || { apk add -q --virtual .busylinux-build openssl; new_key /etc/apk/keys; }

# The build root: Alpine alone, so -dev packages cannot clash with this system's
# (libinput-dev wants eudev-dev, against libudev-zero), and nothing stays behind.
log "Build root $BR"
if [ ! -x "$BR/bin/sh" ]; then
    mkdir -p "$BR/etc/apk"
    grep -v '@busylinux' /etc/apk/repositories > "$BR/etc/apk/repositories"
    apk add -q --root "$BR" --initdb --keys-dir /etc/apk/keys \
        alpine-baselayout alpine-keys busybox busybox-binsh apk-tools musl-utils
fi
cp /etc/resolv.conf "$BR/etc/"
mkdir -p "$BR/dev" "$BR/proc"
mountpoint -q "$BR/dev" || mount --bind /dev "$BR/dev"
mountpoint -q "$BR/proc" || mount -t proc proc "$BR/proc"
chroot "$BR" apk upgrade -q
rm -rf "$BR/build"
mkdir -p "$BR/build/repo"
cp -R "$PKGS" "$BR/build/pkgs"
cp "$TOOLS/lib.sh" "$BR/build/"
install -m600 "$KEY" "$BR/build/key"
mkdir -p "$REPO"

pins=''
for r in "$@"; do
    version=$(field "$r" version) release=$(field "$r" release)
    # One past the installed release, unless meta is ahead.
    case $(installed "$r") in
        "$version"-r*) inst=$(installed "$r")
                       [ "${inst##*-r}" -lt "$release" ] || release=$((${inst##*-r} + 1)) ;;
    esac
    # shellcheck disable=SC2016
    chroot "$BR" /bin/sh -c 'PKGS=/build/pkgs WORK=/build/work SRC=/sources
        REPO=/build/repo KEY=/build/key; . /build/lib.sh; build_pkg "$1" "$2"' sh "$r" "$release"
    mv "$BR/build/repo/"*.apk "$REPO/"
    if [ "$ALL" = 0 ] || [ -n "$(installed "$r")" ]; then pins="$pins $r@busylinux"; fi
done
rm -rf "$BR/build"
index

log "Installing"
# shellcheck disable=SC2086
[ -z "$pins" ] || apk add -u $pins

# apk keeps your edited /etc files and puts its new versions beside them.
new=$(find /etc -name '*.apk-new' 2>/dev/null)
if [ -n "$new" ]; then
    log "New versions wait beside files you changed; merge or delete them:"
    for f in $new; do info "$f"; done
fi
