#!/usr/bin/env bash
# BusyLinux: a musl system built on Alpine Linux edge, with a kernel
# configured from tinyconfig for one specific machine and BusyBox init.
#
# Alpine owns the core: musl, BusyBox, apk-tools and every library come from
# their repositories, so the 15,000 packages in edge/community/testing install
# without any ABI games. This project owns the kernel, the init layer and the
# image.
#
# Environment variables:
#   PACKAGES=           extra Alpine packages to bake into the image
#   BASE_PACKAGES=      override the default package set
#   ALPINE_MIRROR=      default https://dl-cdn.alpinelinux.org/alpine
#   ALPINE_BRANCH=      default edge
#   REPO_URL=           an extra URL where the running system finds this
#                       repository, for pulling kernel updates over the network
#   LOCAL_REPO=0        do not copy this repository into the image
#   VM_SUPPORT=0        drop the virtio/bochs drivers from the kernel
#   MENUCONFIG=1        open menuconfig after the fragments are merged
#   REBUILD=1           rebuild packages even when a cached .apk exists
#   JOBS=N              parallel make jobs (default: nproc)
#   IMAGE_SIZE=8G       size of the (sparse) disk image
#   HOST_UID/HOST_GID   chown the finished files in /build/out to this user
#
# Usage:
#   build.sh                     build everything and write /build/out
#   build.sh --checksum PKG...   refresh a recipe's sha256sums
set -euo pipefail

ARCH=${ARCH:-x86_64}
ALPINE_MIRROR=${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}
ALPINE_BRANCH=${ALPINE_BRANCH:-edge}

# A base that boots, gets on the network and can install the rest.
BASE_PACKAGES=${BASE_PACKAGES:-"alpine-baselayout alpine-keys apk-tools busybox
    busybox-binsh busybox-suid mdev-conf musl-utils amd-ucode
    limine-efi-x86_64 linux-busylinux busylinux-init"}
PACKAGES=${PACKAGES:-}
REPO_URL=${REPO_URL:-}
IMAGE_SIZE=${IMAGE_SIZE:-8G}
ROOT_LABEL=BUSYLINUX_ROOT
LOCAL_REPO_DIR=/var/lib/busylinux/repo
JOBS=${JOBS:-$(nproc)}

# The apk used inside this container. Alpine edge ships the same 3.x series,
# which reads both their v2 repositories and this project's v3 one.
APK_GIT=https://github.com/alpinelinux/apk-tools
APK_REF=v3.0.8

TOP=/build
PKGS=${PKGS_DIR:-/usr/local/share/busylinux/pkgs}
CACHE=$TOP/cache            # sources, built packages, signing key (keep as a volume)
SRC=$CACHE/sources
REPO=$CACHE/packages        # this project's own repository
KEYS=$CACHE/keys
HOSTDIR=$CACHE/host
BLD=$TOP/work
ROOTFS=$TOP/rootfs
OUT=$TOP/out
HOST_PATH=$PATH

export GIT_PAGER=cat PAGER=cat

log()  { printf '\n\033[1;32m==> %s\033[0m\n' "$*" >&2; }
info() { printf '\033[1;34m  > %s\033[0m\n' "$*" >&2; }
die()  { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Recipes
# ---------------------------------------------------------------------------
# pkgs/<name>/{meta,sources,sha256sums,build,files/,split/}; see the README.
# Recipes are built with the container's own toolchain: the kernel needs no
# target libraries, and everything else comes from Alpine.

load_meta() {
    unset version release desc url license depends makedepends subpackages \
          options provides replaces
    release=0
    desc='' url='' license='' depends=''
    subpackages='' options='' provides='' replaces=''
    # shellcheck disable=SC1090
    . "$PKGS/$1/meta"
    [ -n "${version:-}" ] || die "$1: meta sets no version"
    desc=${desc:-$1}
}

recipes() {
    local dir
    for dir in "$PKGS"/*/; do
        [ -f "${dir}meta" ] || continue
        dir=${dir%/}
        printf '%s\n' "${dir##*/}"
    done
}

source_key() {
    case $1 in
        git+*) printf '%s#%s\n' "$(basename "${1%#*}")" "${1##*#}" ;;
        *)     basename "$1" ;;
    esac
}

expected_sum() {
    local key=$2 sum name
    while read -r sum name; do
        [ "$name" = "$key" ] && { printf '%s\n' "$sum"; return 0; }
    done < "$PKGS/$1/sha256sums"
    return 1
}

git_clone() {
    local url=${1%#*} ref=${1##*#}
    rm -rf "$2"
    git -c advice.detachedHead=false clone -q --depth 1 -b "$ref" "$url" "$2"
}

fetch_source() {
    local recipe=$1 src=$2 key file want got
    key=$(source_key "$src")
    case $src in
        git+*|*://*) want=$(expected_sum "$recipe" "$key") ||
            die "$recipe: no checksum for '$key' (run: build.sh --checksum $recipe)" ;;
        *) printf '%s\n' "$PKGS/$recipe/$src"; return 0 ;;
    esac

    case $src in
    git+*)
        file="$SRC/$key.tar.gz"
        if [ ! -s "$file" ]; then
            info "cloning $key"
            git_clone "${src#git+}" "$BLD/clone"
            got=$(git -C "$BLD/clone" rev-parse HEAD)
            [ "$got" = "$want" ] || die "$recipe: $key is commit $got, expected $want"
            rm -rf "$BLD/clone/.git"
            tar -czf "$file.part" -C "$BLD/clone" . && mv "$file.part" "$file"
            rm -rf "$BLD/clone"
        fi
        ;;
    *)
        file="$SRC/$key"
        if [ ! -s "$file" ]; then
            info "downloading $key"
            curl -fsSL --retry 3 -o "$file.part" "$src"
            mv "$file.part" "$file"
        fi
        got=$(sha256sum "$file" | cut -d' ' -f1)
        [ "$got" = "$want" ] || die "$recipe: $key has checksum $got, expected $want"
        ;;
    esac
    printf '%s\n' "$file"
}

prepare_srcdir() {
    local recipe=$1 dir=$2 src dest file target
    while read -r src dest _; do
        case ${src:-} in ''|'#'*) continue ;; esac
        target=$dir${dest:+/$dest}
        mkdir -p "$target"
        file=$(fetch_source "$recipe" "$src")
        case $src in
            git+*)                       tar -xf "$file" -C "$target" ;;
            *.tar|*.tar.*|*.tgz|*.tbz2)  tar -xf "$file" -C "$target" --strip-components=1 ;;
            *://*)                       cp -f "$file" "$target/" ;;
            *)                           cp -Rf "$file" "$target/" ;;
        esac
    done < "$PKGS/$recipe/sources"
}

checksum_recipe() {
    local recipe=$1 src key file sums=
    while read -r src _; do
        case ${src:-} in ''|'#'*) continue ;; esac
        key=$(source_key "$src")
        case $src in
        git+*)
            git_clone "${src#git+}" "$BLD/clone"
            sums+="$(git -C "$BLD/clone" rev-parse HEAD)  $key"$'\n'
            rm -rf "$BLD/clone"
            ;;
        *://*)
            file="$SRC/$key"
            [ -s "$file" ] || curl -fsSL --retry 3 -o "$file" "$src"
            sums+="$(sha256sum "$file" | cut -d' ' -f1)  $key"$'\n'
            ;;
        esac
    done < "$PKGS/$recipe/sources"
    printf '%s' "$sums" > "$PKGS/$recipe/sha256sums"
    info "$recipe: wrote $(grep -c . "$PKGS/$recipe/sha256sums") checksums"
}

# ---------------------------------------------------------------------------
# apk plumbing
# ---------------------------------------------------------------------------
build_host_apk() {
    APK=$HOSTDIR/bin/apk
    if [ -x "$APK" ]; then
        info "using cached $("$APK" --version 2>&1 | tr -d ,)"
        return
    fi
    log "Building apk-tools $APK_REF for the container"
    rm -rf "$BLD/apk-host"
    git_clone "$APK_GIT#$APK_REF" "$BLD/apk-host"
    ( cd "$BLD/apk-host"
      meson setup --prefix=/usr -Ddefault_library=static -Dlua=disabled \
          -Dpython=disabled -Dtests=disabled -Ddocs=disabled build > /dev/null
      ninja -C build > /dev/null )
    mkdir -p "$HOSTDIR/bin"
    install -m 755 "$BLD/apk-host/build/src/apk" "$APK"
    rm -rf "$BLD/apk-host"
}

setup_key() {
    mkdir -p "$KEYS/pub"
    KEY=$KEYS/busylinux.rsa
    KEYPUB=$KEYS/pub/busylinux.rsa.pub
    [ -f "$KEY" ] && return
    log "Generating this repository's signing key"
    openssl genrsa -out "$KEY" 4096 2>/dev/null
    chmod 600 "$KEY"
    openssl rsa -in "$KEY" -pubout -out "$KEYPUB" 2>/dev/null
    info "private key: ${KEY#"$TOP"/} inside the build cache - keep it"
}

# Alpine's signing keys, fetched over HTTPS (the container verifies the
# certificate) and then used to verify every package apk installs.
fetch_alpine_keys() {
    local index=$BLD/APKINDEX version
    [ -f "$KEYS/pub/alpine-devel@lists.alpinelinux.org-4a6a0840.rsa.pub" ] && {
        info "Alpine keys already in the build keyring"; return; }
    log "Fetching Alpine's signing keys"
    curl -fsSL "$ALPINE_MIRROR/$ALPINE_BRANCH/main/$ARCH/APKINDEX.tar.gz" |
        tar -xzO APKINDEX > "$index"
    version=$(awk '/^P:alpine-keys$/{f=1} f&&/^V:/{print substr($0,3); exit}' "$index")
    [ -n "$version" ] || die "alpine-keys not found in the $ALPINE_BRANCH index"
    curl -fsSL -o "$BLD/alpine-keys.apk" \
        "$ALPINE_MIRROR/$ALPINE_BRANCH/main/$ARCH/alpine-keys-$version.apk"
    rm -rf "$BLD/keys"; mkdir -p "$BLD/keys"
    tar -xzf "$BLD/alpine-keys.apk" -C "$BLD/keys" 2>/dev/null || true
    cp "$BLD/keys"/etc/apk/keys/*.pub "$KEYS/pub/"
    info "alpine-keys $version: $(ls "$BLD/keys"/etc/apk/keys | wc -l) keys"
}

apk_host() { "$APK" --keys-dir "$KEYS/pub" "$@"; }

reindex() {
    apk_host mkndx --output "$REPO/$ARCH/Packages.adb" --sign-key "$KEY" \
        "$REPO/$ARCH"/*.apk > /dev/null
}

# Repositories used while building the image. Plain http: apk verifies every
# package against the keys above, and an empty root has no CA bundle yet.
install_repos() {
    local base=${ALPINE_MIRROR/https:/http:}
    # "v3 <base>" makes apk look for <base>/<arch>/Packages.adb.
    printf 'v3 %s\n%s/%s/main\n%s/%s/community\n@testing %s/%s/testing\n' \
        "$REPO" \
        "$base" "$ALPINE_BRANCH" "$base" "$ALPINE_BRANCH" "$base" "$ALPINE_BRANCH"
}

apk_root() {
    local root=$1; shift
    mkdir -p "$root/etc/apk/keys"
    cp -f "$KEYS"/pub/*.pub "$root/etc/apk/keys/"
    install_repos > "$BLD/repositories"
    apk_host --root "$root" --repositories-file "$BLD/repositories" "$@"
}

# ---------------------------------------------------------------------------
# Building this project's packages
# ---------------------------------------------------------------------------
strip_tree() {
    local dir=$1 file
    while IFS= read -r -d '' file; do
        head -c 4 "$file" 2>/dev/null | grep -q $'\x7fELF' || continue
        case $file in
            *.ko|*.ko.*)  continue ;;
            *.a|*.o)      strip -g "$file" 2>/dev/null || : ;;
            *)            strip -s -R .comment -R .note "$file" 2>/dev/null || : ;;
        esac
    done < <(find "$dir" -type f -print0)
}

make_package() {
    local name=$1 pkgver=$2 pdesc=$3 pdeps=$4 dir=$5
    local out="$REPO/$ARCH/$name-$pkgver.apk" script
    local -a args=()
    mkdir -p "$REPO/$ARCH"
    # One space-separated value: repeated --info depends: flags overwrite.
    [ -n "$pdeps" ] && args+=(--info "depends:$pdeps")
    [ -n "$provides" ] && args+=(--info "provides:$provides")
    [ -n "$replaces" ] && args+=(--info "replaces:$replaces")
    for script in "$PKGS/$RECIPE/scripts/$name".*; do
        [ -f "$script" ] && args+=(--script "${script##*.}:$script")
    done
    apk_host mkpkg \
        --files "$dir" \
        --info "name:$name" --info "version:$pkgver" \
        --info "description:$pdesc" --info "arch:$ARCH" \
        --info "license:${license:-custom}" --info "url:$url" \
        --info "origin:$RECIPE" \
        "${args[@]}" --sign-key "$KEY" --output "$out" > /dev/null
    info "$name-$pkgver.apk ($(du -sh "$dir" | cut -f1) installed)"
}

build_recipe() {
    RECIPE=$1
    load_meta "$RECIPE"
    local pkgver="$version-r$release" sub subdesc subdeps var
    local dir="$BLD/$RECIPE" destdir="$BLD/$RECIPE/pkg" srcdir="$BLD/$RECIPE/src"

    if [ -f "$REPO/$ARCH/$RECIPE-$pkgver.apk" ] && [ "${REBUILD:-0}" != 1 ]; then
        log "$RECIPE $pkgver: cached"
        return
    fi
    log "$RECIPE $pkgver: building"
    rm -rf "$dir"
    mkdir -p "$srcdir" "$destdir"
    prepare_srcdir "$RECIPE" "$srcdir"
    ( cd "$srcdir" && sh -e "$PKGS/$RECIPE/build" "$destdir" "$version" ) ||
        die "$RECIPE: build failed"
    case " $options " in *" nostrip "*) ;; *) strip_tree "$destdir" ;; esac

    for sub in $subpackages; do
        mkdir -p "$dir/sub-$sub"
        sh -e "$PKGS/$RECIPE/split/$sub" "$destdir" "$dir/sub-$sub" ||
            die "$RECIPE: splitting $sub failed"
    done

    make_package "$RECIPE" "$pkgver" "$desc" "$depends" "$destdir"
    for sub in $subpackages; do
        var=${sub//-/_}
        eval "subdesc=\${${var}_desc:-\"\$desc (${sub##*-} files)\"}"
        eval "subdeps=\${${var}_depends:-\"\$RECIPE=\$pkgver\"}"
        make_package "$sub" "$pkgver" "$subdesc" "$subdeps" "$dir/sub-$sub"
    done
    rm -rf "$srcdir" "$destdir" "$dir"/sub-*
}

# ---------------------------------------------------------------------------
# Image
# ---------------------------------------------------------------------------
# A copy of this project's repository inside the image. Without it apk has two
# installed packages -- the kernel and the init layer -- that no repository
# offers, which it reports on every run and which "apk upgrade --prune" would
# delete. It also gives the machine somewhere to roll a kernel back to.
# Only the newest release of each package goes in: the build cache keeps every
# kernel ever built and they are 9 MB each.
install_local_repo() {
    local root=$1 dir apk name newest
    [ "${LOCAL_REPO:-1}" = 1 ] || return 0
    dir=$root$LOCAL_REPO_DIR/$ARCH
    rm -rf "${root:?}$LOCAL_REPO_DIR"
    mkdir -p "$dir"
    for apk in "$REPO/$ARCH"/*.apk; do
        name=$(basename "$apk" | sed 's/-[^-]*-r[0-9]*\.apk$//')
        newest=$(find "$REPO/$ARCH" -maxdepth 1 -name "$name-*.apk" -printf '%f\n' |
                 grep -E "^$name-[^-]+-r[0-9]+\.apk$" | sort -V | tail -1)
        [ -f "$dir/$newest" ] || cp "$REPO/$ARCH/$newest" "$dir/"
    done
    apk_host mkndx --output "$dir/Packages.adb" --sign-key "$KEY" "$dir"/*.apk > /dev/null
    info "local repository: $LOCAL_REPO_DIR ($(du -sh "$dir" | cut -f1), $(ls "$dir"/*.apk | wc -l) packages)"
}

write_config() {
    local root=$1 repo_line='#v3 https://example.org/busylinux'
    [ "${LOCAL_REPO:-1}" = 1 ] && repo_line="v3 $LOCAL_REPO_DIR"
    [ -n "$REPO_URL" ] && repo_line="$repo_line"$'\n'"v3 $REPO_URL"
    cat > "$root/etc/apk/repositories" <<EOF
# This project's repository: the kernel and the init layer.
$repo_line

# Alpine $ALPINE_BRANCH. 'testing' is tagged, so it is used only when asked
# for by name: apk add cliphist@testing
$ALPINE_MIRROR/$ALPINE_BRANCH/main
$ALPINE_MIRROR/$ALPINE_BRANCH/community
@testing $ALPINE_MIRROR/$ALPINE_BRANCH/testing
EOF
    echo busylinux > "$root/etc/hostname"
    # Root starts without a password; set one on first boot with passwd.
    sed -i 's/^root:[^:]*:/root::/' "$root/etc/shadow"
}

build_initramfs() {
    local initrd=$BLD/initramfs lib
    rm -rf "$initrd"
    mkdir -p "$initrd"/bin "$initrd"/dev "$initrd"/proc "$initrd"/sys \
             "$initrd"/newroot "$initrd"/lib
    install -m 755 "$ROOTFS/bin/busybox" "$initrd/bin/busybox"
    for lib in $(readelf -d "$ROOTFS/bin/busybox" |
                 sed -n 's/.*Shared library: \[\(.*\)\]/\1/p') \
               ld-musl-$ARCH.so.1; do
        [ -e "$ROOTFS/lib/$lib" ] && cp -L "$ROOTFS/lib/$lib" "$initrd/lib/"
    done
    mknod -m 600 "$initrd/dev/console" c 5 1
    mknod -m 666 "$initrd/dev/null"    c 1 3
    cp "$PKGS/busylinux-init/files/initramfs-init" "$initrd/init"
    chmod 755 "$initrd/init"
    ( cd "$initrd" && find . -print0 | cpio --null -o -H newc -R 0:0 --quiet ) |
        gzip -9 > "$OUT/initramfs.cpio.gz"
}

# ---------------------------------------------------------------------------
main() {
    mkdir -p "$CACHE" "$SRC" "$KEYS" "$REPO/$ARCH" "$OUT" "$BLD"

    if [ "${1:-}" = --checksum ]; then
        shift
        for recipe in "$@"; do checksum_recipe "$recipe"; done
        exit 0
    fi

    log "Preparing"
    rm -rf "$ROOTFS" "$BLD/initramfs" "$BLD/iso"
    mkdir -p "$ROOTFS"
    build_host_apk
    setup_key
    fetch_alpine_keys
    export JOBS ARCH MENUCONFIG=${MENUCONFIG:-0} VM_SUPPORT=${VM_SUPPORT:-1}
    export HOSTCC=/usr/bin/gcc MAKEFLAGS="-j$JOBS"

    for recipe in $(recipes); do build_recipe "$recipe"; done
    reindex

    log "Installing the image from Alpine $ALPINE_BRANCH"
    # shellcheck disable=SC2086
    apk_root "$ROOTFS" add --initdb $BASE_PACKAGES $PACKAGES
    install_local_repo "$ROOTFS"
    write_config "$ROOTFS"
    # Real nodes, so that the checks below (and anything else redirecting to
    # /dev/null before devtmpfs is mounted) do not leave a regular file there.
    mkdir -p "$ROOTFS/dev"
    rm -f "$ROOTFS/dev/null" "$ROOTFS/dev/console"
    mknod -m 666 "$ROOTFS/dev/null"    c 1 3
    mknod -m 600 "$ROOTFS/dev/console" c 5 1

    log "Checking the image"
    PATH=$HOST_PATH chroot "$ROOTFS" /bin/busybox sh -ec '
        . /etc/profile
        echo "busybox: $(busybox | sed -n 1p | cut -d, -f1)"
        echo "libc:    $(ls /lib/ld-musl-*.so.1)"
        echo "apk:     $(apk --version)"
        printf "packages: "; apk list --installed 2>/dev/null | wc -l
        echo "kernel:  $(ls /lib/modules)"
        [ -x /sbin/init ] || { echo "no /sbin/init" >&2; exit 1; }
        echo "image OK"'

    log "Kernel and initramfs"
    cp "$ROOTFS/boot/vmlinuz-busylinux" "$OUT/vmlinuz"
    # Early CPU microcode, loaded as the first initrd by the boot loader.
    [ -f "$ROOTFS/boot/amd-ucode.img" ] && cp "$ROOTFS/boot/amd-ucode.img" "$OUT/"
    build_initramfs
    info "kernel $(ls "$ROOTFS/lib/modules"), $(du -h "$OUT/vmlinuz" | cut -f1) vmlinuz"

    log "Creating the disk image ($IMAGE_SIZE, ext4, label $ROOT_LABEL)"
    rm -f "$OUT/disk.img"
    truncate -s "$IMAGE_SIZE" "$OUT/disk.img"
    mke2fs -q -t ext4 -L "$ROOT_LABEL" -d "$ROOTFS" "$OUT/disk.img"

    log "Creating rootfs.tar.gz"
    tar -C "$ROOTFS" -czf "$OUT/rootfs.tar.gz" .

    log "Creating the boot ISO (UEFI)"
    local iso=$BLD/iso limcd=$BLD/limine-cd
    mkdir -p "$iso/EFI/BOOT"
    cp "$OUT/vmlinuz" "$OUT/initramfs.cpio.gz" "$iso/"
    local ucode=''
    if [ -f "$OUT/amd-ucode.img" ]; then
        cp "$OUT/amd-ucode.img" "$iso/"
        # The microcode has to be the first module the kernel is handed.
        ucode="    module_path: boot():/amd-ucode.img"$'\n'
    fi
    # Limine's own EFI application comes from the image; the El Torito FAT
    # image it needs for a CD is 3 MB and would be dead weight there, so that
    # one package goes into a throwaway root.
    cp "$ROOTFS/usr/share/limine/BOOTX64.EFI" "$iso/EFI/BOOT/"
    rm -rf "$limcd"
    apk_root "$limcd" add --initdb limine-efi-cd > /dev/null
    cat > "$iso/EFI/BOOT/limine.conf" <<EOF
timeout: 3
default_entry: 1
serial: yes

/BusyLinux (root=LABEL=$ROOT_LABEL)
    protocol: linux
    path: boot():/vmlinuz
    cmdline: root=LABEL=$ROOT_LABEL rw console=tty0 console=ttyS0,115200
$ucode    module_path: boot():/initramfs.cpio.gz

/Rescue shell (initramfs only)
    protocol: linux
    path: boot():/vmlinuz
    cmdline: rescue console=tty0 console=ttyS0,115200
$ucode    module_path: boot():/initramfs.cpio.gz
EOF
    cp "$limcd/usr/share/limine/limine-uefi-cd.bin" "$iso/"
    rm -f "$OUT/busylinux.iso"
    PATH=$HOST_PATH xorriso -as mkisofs -R -r -J -quiet \
        -V BUSYLINUX \
        --efi-boot limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
        --protective-msdos-label "$iso" -o "$OUT/busylinux.iso"
    rm -rf "$limcd"

    log "Publishing this project's repository"
    rm -rf "$OUT/repo"; mkdir -p "$OUT/repo"
    cp -R "$REPO/$ARCH" "$OUT/repo/"
    cp "$KEYPUB" "$OUT/repo/"

    [ -n "${HOST_UID:-}" ] && chown -R "$HOST_UID:${HOST_GID:-$HOST_UID}" "$OUT"

    log "Done"
    printf 'image: %s, %s packages\n' "$(du -sh "$ROOTFS" | cut -f1)" \
        "$(apk_root "$ROOTFS" list --installed | wc -l)"
    ls -lsh "$OUT"
}

main "$@"
