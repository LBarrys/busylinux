#!/bin/sh
set -eu

usage() {
    cat <<'EOT'
Builds this repository's packages on the machine itself and installs them.

    update.sh [--yes] recipe...     the named recipes, e.g. rtkit or linux-busylinux
    update.sh [--yes] --all         every recipe
    update.sh                       list the recipes

Run as root from a checkout. Build tools are installed for the build and
removed afterwards. Packages are signed with /root/keys/local.rsa (created on
first use) and pinned to this repository: apk add NAME@busylinux. Changed /etc
files are kept; the new versions land beside them as .apk-new.
EOT
}

PKGS=$(cd "$(dirname "$0")" && pwd -P)/pkgs
WORK=/var/tmp/busylinux-update SRC=/var/tmp/busylinux-update/sources
REPO=/var/lib/busylinux/repo/x86_64 KEY=/root/keys/local.rsa
# shellcheck source=lib.sh
. "${PKGS%/pkgs}/lib.sh"

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
trap 'apk del -q .busylinux-build 2>/dev/null || :' EXIT

# This repository is tagged, so apk never swaps its packages for Alpine's.
if grep -qx "v3 ${REPO%/*}" /etc/apk/repositories; then
    sed -i "s|^v3 ${REPO%/*}\$|v3 @busylinux ${REPO%/*}|" /etc/apk/repositories
    for r in $(recipes); do
        if grep -qx "$r" /etc/apk/world; then sed -i "s/^$r\$/$r@busylinux/" /etc/apk/world; fi
    done
fi
[ -f "$KEY" ] || { apk add -q --virtual .busylinux-build openssl; new_key /etc/apk/keys; }

pins=''
for r in "$@"; do
    version=$(field "$r" version) release=$(field "$r" release)
    # One past the installed release, unless meta is ahead.
    case $(installed "$r") in
        "$version"-r*) inst=$(installed "$r")
                       [ "${inst##*-r}" -lt "$release" ] || release=$((${inst##*-r} + 1)) ;;
    esac
    build_pkg "$r" "$release"
    if [ "$ALL" = 0 ] || [ -n "$(installed "$r")" ]; then pins="$pins $r@busylinux"; fi
done
index

log "Installing"
# shellcheck disable=SC2086
[ -z "$pins" ] || apk add -u $pins
