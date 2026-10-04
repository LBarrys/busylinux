# shellcheck shell=sh
# Shared by build.sh and update.sh: builds a recipe in pkgs/ into a signed .apk.

die()  { printf '\033[1;31merror: %s\033[0m\n' "$*" >&2; exit 1; }
log()  { printf '\n\033[1;32m==> %s\033[0m\n' "$*" >&2; }
info() { printf '\033[1;34m  > %s\033[0m\n' "$*" >&2; }

# A meta field, read in a subshell so recipes cannot leak into each other.
# shellcheck disable=SC2034
field() (
    version='' release=0 desc='' url='' license='' depends='' makedepends=''
    provides='' replaces='' provider_priority='' options=''
    # shellcheck source=/dev/null
    . "$PKGS/$1/meta"
    eval "printf '%s\n' \"\$$2\""
)

recipes() {
    for d in "$PKGS"/*/meta; do d=${d%/meta}; printf '%s\n' "${d##*/}"; done
}

# Copies a recipe's files/ into $2, then fetches, checks and unpacks its sources.
fetch_sources() {
    [ ! -d "$PKGS/$1/files" ] || cp -R "$PKGS/$1/files/." "$2/"
    [ -f "$PKGS/$1/sources" ] || return 0
    while read -r src dest; do
        case ${src:-} in ''|'#'*) continue ;; esac
        target=$2${dest:+/$dest}
        mkdir -p "$target"
        case $src in
        git+*://*|*://*)
            url=${src#git+} ref=${src##*#}
            case $src in git+*) url=${url%#*} key=${url##*/}#$ref ;; *) key=${src##*/} ;; esac
            want=$(awk -v k="$key" '$2 == k { print $1; exit }' "$PKGS/$1/sha256sums")
            case $src in
            git+*)
                rm -rf "$WORK/clone"
                git -c advice.detachedHead=false clone -q --depth 1 -b "$ref" "$url" "$WORK/clone"
                got=$(git -C "$WORK/clone" rev-parse HEAD)
                rm -rf "$WORK/clone/.git"
                cp -a "$WORK/clone/." "$target/"
                rm -rf "$WORK/clone" ;;
            *)
                file=$SRC/$key
                [ -s "$file" ] || { wget -q -O "$file.part" "$src" && mv "$file.part" "$file"; } ||
                    die "$1: could not download $src"
                got=$(sha256sum "$file" | cut -d' ' -f1)
                case $src in
                    *.tar|*.tar.*|*.tgz) tar -xf "$file" -C "$target" --strip-components=1 ;;
                    *)                   cp "$file" "$target/" ;;
                esac ;;
            esac
            [ "$got" = "$want" ] ||
                die "$1: $key is ${want:+not $want but }$got; pkgs/$1/sha256sums needs '$got  $key'" ;;
        *)  die "$1: sources lists '$src'; files/ is copied by itself" ;;
        esac
    done < "$PKGS/$1/sources"
}

strip_tree() {
    command -v strip > /dev/null || return 0
    find "$1" -type f | while read -r f; do
        case $(head -c 4 "$f" | tr -d '\000') in *ELF) ;; *) continue ;; esac
        case $f in
            *.ko|*.ko.*) ;;
            *.a|*.o)     strip -g "$f" || : ;;
            *)           strip -s -R .comment -R .note "$f" || : ;;
        esac
    done
}

# build_pkg RECIPE RELEASE: builds $REPO/RECIPE-VERSION-rRELEASE.apk, signed with $KEY.
build_pkg() {
    r=$1 version=$(field "$1" version)
    pkgver=$version-r$2 dir=$WORK/$r
    log "$r $pkgver"
    rm -rf "$dir"
    mkdir -p "$dir/src" "$dir/pkg" "$SRC" "$REPO"

    deps=$(field "$r" makedepends)
    ! grep -qs '^git+' "$PKGS/$r/sources" || deps="$deps git"
    # shellcheck disable=SC2086
    [ -z "${deps# }" ] || apk add -q --virtual .busylinux-build $deps ||
        die "$r: could not install $deps"

    fetch_sources "$r" "$dir/src"
    ( cd "$dir/src" && MAKEFLAGS=-j$(nproc) sh -e "$PKGS/$r/build" "$dir/pkg" "$version" ) ||
        die "$r: build failed"
    case " $(field "$r" options) " in *" nostrip "*) ;; *) strip_tree "$dir/pkg" ;; esac
    [ -z "${deps# }" ] || apk del -q .busylinux-build

    set -- --info "name:$r" --info "version:$pkgver" --info "arch:$(apk --print-arch)" \
        --info "description:$(field "$r" desc)" --info "license:$(field "$r" license)" \
        --info "url:$(field "$r" url)" --info "origin:$r"
    for k in depends provides replaces provider_priority; do
        v=$(field "$r" $k)
        [ -z "$v" ] || set -- "$@" --info "$(echo "$k" | tr _ -):$v"
    done
    for s in "$PKGS/$r/scripts/$r".*; do
        [ -f "$s" ] && set -- "$@" --script "${s##*.}:$s"
    done
    apk mkpkg --files "$dir/pkg" "$@" --sign-key "$KEY" --output "$REPO/$r-$pkgver.apk"
    info "$(du -h "$REPO/$r-$pkgver.apk" | cut -f1) $r-$pkgver.apk"
    rm -rf "$dir"
}

index() {
    apk mkndx --output "$REPO/Packages.adb" --sign-key "$KEY" "$REPO"/*.apk > /dev/null
}

new_key() {
    [ -f "$KEY" ] && return
    mkdir -p "${KEY%/*}"
    openssl genrsa -out "$KEY" 4096 2>/dev/null
    chmod 600 "$KEY"
    openssl rsa -in "$KEY" -pubout -out "$1/${KEY##*/}.pub" 2>/dev/null
    info "generated $KEY"
}
