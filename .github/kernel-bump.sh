#!/bin/sh
# Points pkgs/linux-busylinux at the newest release of its series, after
# checking the tarball's signature by Linus Torvalds or Greg Kroah-Hartman.
# Prints the new version, or nothing when the recipe is current.
set -eu

R=pkgs/linux-busylinux
old=$(sed -n 's/^version=//p' $R/meta)
series=$(echo "$old" | cut -d. -f1,2)
new=$(curl -fsS https://www.kernel.org/releases.json |
      sed -n "s/.*\"version\": *\"\($(echo "$series" | sed 's/\./\\./g')\(\.[0-9]*\)\{0,1\}\)\".*/\1/p" |
      head -1)
[ -n "$new" ] || { echo "kernel.org no longer lists $series" >&2; exit 1; }
[ "$new" != "$old" ] || exit 0

url=https://cdn.kernel.org/pub/linux/kernel/v${new%%.*}.x/linux-$new
wkd=https://kernel.org/.well-known/openpgpkey/hu
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsS -o "$tmp/linux.tar.xz" "$url.tar.xz"
curl -fsS -o "$tmp/linux.tar.sign" "$url.tar.sign"
curl -fsS "$wkd/pf113mfnx1f3eb1yiwhsipa91xfc7o4x?l=torvalds" > "$tmp/keys.gpg"
curl -fsS "$wkd/e3n9xnm94c5apezqnj1pmrfuaoyfm8cf?l=gregkh" >> "$tmp/keys.gpg"
xz -dc "$tmp/linux.tar.xz" |
    gpgv --homedir "$tmp" --keyring "$tmp/keys.gpg" --status-fd 1 "$tmp/linux.tar.sign" - 2>/dev/null |
    grep -Eq '^\[GNUPG:\] VALIDSIG .* (ABAF11C65A2970B130ABE3C479BE3E4300411886|647F28654894E3BD457199BE38DBBDC86092693E)$' ||
    { echo "linux-$new.tar.xz: no good signature" >&2; exit 1; }

sed -i "s/^version=.*/version=$new/; s/^release=.*/release=0/" $R/meta
echo "$url.tar.xz" > $R/sources
echo "$(sha256sum "$tmp/linux.tar.xz" | cut -d' ' -f1)  linux-$new.tar.xz" > $R/sha256sums
echo "$new"
