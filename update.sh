#!/bin/sh
set -eu

usage() {
    cat <<'EOT'
Build this repository's packages on the machine itself and install them.

    update.sh [options] recipe...
    update.sh [options] --all

Runs as root from a checkout of this repository. Builds the named recipes in
pkgs/ (--all: every one but the kernel, which kernel-update.sh handles), signs
the packages, adds them to the local repository and upgrades the ones that are
installed. Without a name it lists the recipes. Files under /etc that you have
changed are kept; apk puts the new version beside them as .apk-new.

Options:
    --all           every recipe but the kernel
    --workdir DIR   build directory (default: /var/tmp/busylinux-update)
    --key FILE      signing key (default: /root/keys/local.rsa, created)
    --repo DIR      local repository (default: /var/lib/busylinux/repo/x86_64)
    --no-install    build and add to the repository only
    --keep-tools    leave build-base, makedepends and the like installed; by
                    default every package this run installed is removed again
    --yes           do not ask for confirmation
EOT
}

WORK=/var/tmp/busylinux-update KEY=/root/keys/local.rsa
REPO=/var/lib/busylinux/repo/x86_64
INSTALL=1 KEEP_TOOLS=0 ASSUME_YES=0 ALL=0 ADDED=''

die()  { printf '\033[1;31merror: %s\033[0m\n' "$*" >&2; exit 1; }
log()  { printf '\n\033[1;32m==> %s\033[0m\n' "$*"; }
info() { printf '\033[1;34m  > %s\033[0m\n' "$*"; }

while [ $# -gt 0 ]; do
    case $1 in
        --workdir)    WORK=$2; shift 2 ;;
        --key)        KEY=$2; shift 2 ;;
        --repo)       REPO=$2; shift 2 ;;
        --no-install) INSTALL=0; shift ;;
        --keep-tools) KEEP_TOOLS=1; shift ;;
        --all)        ALL=1; shift ;;
        --yes|-y)     ASSUME_YES=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        -*)           die "unknown option: $1" ;;
        *)            break ;;
    esac
done

[ "$(id -u)" = 0 ] || die "run this as root"

PKGS=$(cd "$(dirname "$0")" && pwd -P)/pkgs
[ -d "$PKGS" ] || die "no pkgs/ beside $0; run this from a checkout"

# A meta field, read in a subshell so recipes cannot leak into each other.
# shellcheck disable=SC2034
field() (
    version='' release=0 desc='' url='' license='' depends='' makedepends=''
    provides='' replaces='' provider_priority='' options='' subpackages=''
    # shellcheck source=/dev/null
    . "$PKGS/$1/meta"
    eval "printf '%s\n' \"\$$2\""
)

installed() {
    apk list --installed "$1" 2>/dev/null | sed -n "s/^$1-\([0-9][^ ]*\) .*/\1/p" | head -1
}

remove_tools() {
    [ -n "$ADDED" ] || return 0
    if [ "$KEEP_TOOLS" = 1 ]; then
        info "left installed:$ADDED (--keep-tools)"
        return 0
    fi
    log "Removing what this run installed"
    # shellcheck disable=SC2086
    apk del $ADDED > /dev/null 2>&1 && info "removed:$ADDED" ||
        info "could not remove all of:$ADDED -- something else needs them"
}

recipes() {
    for d in "$PKGS"/*/; do
        d=${d%/}; d=${d##*/}
        [ "$d" = linux-busylinux ] || printf '%s\n' "$d"
    done
}

if [ $# -eq 0 ] && [ "$ALL" = 1 ]; then
    # shellcheck disable=SC2046
    set -- $(recipes)
elif [ $# -eq 0 ]; then
    log "Recipes (version in pkgs/, installed)"
    for r in $(recipes); do
        info "$r $(field "$r" version)-r$(field "$r" release), installed: $(installed "$r" || :)"
    done
    die "name the recipes to update, or pass --all"
fi
for r in "$@"; do
    [ -f "$PKGS/$r/meta" ] || die "no recipe pkgs/$r"
    [ "$r" != linux-busylinux ] || die "use kernel-update.sh for the kernel"
done

log "BusyLinux packages: $*"
for r in "$@"; do
    info "$r $(field "$r" version), installed: $(installed "$r" || :)"
done
if [ "$ASSUME_YES" != 1 ]; then
    printf 'Continue [y/N]? '
    read -r answer
    case $answer in y|Y|yes) ;; *) die "not confirmed" ;; esac
fi

log "Checking the tools"
want=''
for r in "$@"; do
    md=$(field "$r" makedepends)
    [ -z "$md" ] || want="$want build-base $md"
    grep -q '^git+' "$PKGS/$r/sources" 2>/dev/null && want="$want git"
done
[ -f "$KEY" ] || want="$want openssl"
for t in $want; do
    case " $ADDED " in *" $t "*) continue ;; esac
    apk info -e "$t" > /dev/null 2>&1 || ADDED="$ADDED $t"
done
trap remove_tools EXIT
if [ -n "$ADDED" ]; then
    info "installing:$ADDED"
    # shellcheck disable=SC2086
    apk add $ADDED > /dev/null
else
    info "nothing to install"
fi

if [ ! -f "$KEY" ]; then
    mkdir -p "$(dirname "$KEY")"
    openssl genrsa -out "$KEY" 4096 2>/dev/null
    chmod 600 "$KEY"
    openssl rsa -in "$KEY" -pubout -out "/etc/apk/keys/$(basename "$KEY").pub" 2>/dev/null
    info "generated $KEY and trusted its public half"
fi

fetch_sources() (
    dir=$PKGS/$1 srcdir=$2
    while read -r src dest; do
        case ${src:-} in ''|'#'*) continue ;; esac
        target=$srcdir${dest:+/$dest}
        mkdir -p "$target"
        case $src in
        git+*)
            url=${src#git+} ref=${src##*#}
            url=${url%#*}
            key=$(basename "$url")#$ref
            want=$(awk -v k="$key" '$2 == k { print $1; exit }' "$dir/sha256sums")
            [ -n "$want" ] || die "$1: no checksum for $key"
            rm -rf "$WORK/clone"
            git -c advice.detachedHead=false clone -q --depth 1 -b "$ref" "$url" "$WORK/clone"
            got=$(git -C "$WORK/clone" rev-parse HEAD)
            [ "$got" = "$want" ] || die "$1: $key is commit $got, expected $want"
            rm -rf "$WORK/clone/.git"
            cp -a "$WORK/clone/." "$target/"
            rm -rf "$WORK/clone"
            ;;
        *://*)
            key=$(basename "$src") file=$WORK/$(basename "$src")
            want=$(awk -v k="$key" '$2 == k { print $1; exit }' "$dir/sha256sums")
            [ -n "$want" ] || die "$1: no checksum for $key"
            [ -s "$file" ] || { wget -q -O "$file.part" "$src" && mv "$file.part" "$file"; } ||
                die "$1: could not download $src"
            got=$(sha256sum "$file" | cut -d' ' -f1)
            [ "$got" = "$want" ] || die "$1: $key has checksum $got, expected $want"
            case $src in
                *.tar|*.tar.*|*.tgz|*.tbz2) tar -xf "$file" -C "$target" --strip-components=1 ;;
                *)                          cp -f "$file" "$target/" ;;
            esac
            ;;
        *)  cp -R "$dir/$src" "$target/" ;;
        esac
    done < "$dir/sources"
)

strip_tree() {
    command -v strip > /dev/null 2>&1 || return 0
    find "$1" -type f | while read -r f; do
        case $(head -c 4 "$f" 2>/dev/null | tr -d '\000') in *ELF) ;; *) continue ;; esac
        case $f in
            *.ko|*.ko.*) ;;
            *.a|*.o)     strip -g "$f" 2>/dev/null || : ;;
            *)           strip -s -R .comment -R .note "$f" 2>/dev/null || : ;;
        esac
    done
}

info_fields() {
    printf '%s\n' "name:$1" "version:$2" "description:$(field "$1" desc)" \
        "arch:$ARCH" "license:$(field "$1" license)" "url:$(field "$1" url)" "origin:$1"
    for k in depends provides replaces; do
        v=$(field "$1" "$k")
        [ -z "$v" ] || printf '%s:%s\n' "$k" "$v"
    done
    v=$(field "$1" provider_priority)
    [ -z "$v" ] || printf 'provider-priority:%s\n' "$v"
}

ARCH=$(apk --print-arch)
mkdir -p "$WORK" "$REPO"
UPGRADE='' NEW=''

for r in "$@"; do
    version=$(field "$r" version) release=$(field "$r" release)
    [ -z "$(field "$r" subpackages)" ] || die "$r: subpackages are built by build.sh only"
    # One past the installed release, unless meta is ahead.
    inst=$(installed "$r" || :)
    case $inst in
        "$version"-r*) [ "${inst##*-r}" -lt "$release" ] || release=$(( ${inst##*-r} + 1 )) ;;
    esac
    pkgver=$version-r$release

    log "$r $pkgver"
    src=$WORK/$r/src pkg=$WORK/$r/pkg
    rm -rf "${WORK:?}/$r"
    mkdir -p "$src" "$pkg"
    fetch_sources "$r" "$src"
    ( cd "$src" && sh -e "$PKGS/$r/build" "$pkg" "$version" ) || die "$r: build failed"
    case " $(field "$r" options) " in *" nostrip "*) ;; *) strip_tree "$pkg" ;; esac

    # "$@" now holds apk mkpkg's arguments.
    set --
    while IFS= read -r i; do set -- "$@" --info "$i"; done <<EOF
$(info_fields "$r" "$pkgver")
EOF
    for s in "$PKGS/$r/scripts/$r".*; do
        [ -f "$s" ] && set -- "$@" --script "${s##*.}:$s"
    done
    apk mkpkg --files "$pkg" "$@" --sign-key "$KEY" --output "$REPO/$r-$pkgver.apk"
    info "$(du -h "$REPO/$r-$pkgver.apk" | cut -f1) $REPO/$r-$pkgver.apk"
    rm -rf "${WORK:?}/$r"

    if [ -n "$inst" ]; then UPGRADE="$UPGRADE $r"; else NEW="$NEW $r"; fi
done

apk mkndx --output "$REPO/Packages.adb" --sign-key "$KEY" "$REPO"/*.apk > /dev/null

if [ "$INSTALL" = 1 ] && [ -n "$UPGRADE" ]; then
    log "Upgrading:$UPGRADE"
    # shellcheck disable=SC2086
    apk upgrade $UPGRADE
fi

log "Done"
[ "$INSTALL" = 1 ] || [ -z "$UPGRADE" ] || info "install with: apk upgrade$UPGRADE"
[ -z "$NEW" ] || info "not installed, now available:$NEW (apk add$NEW)"
