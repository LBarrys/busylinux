#!/bin/sh
set -eu

usage() {
    cat <<'EOT'
Installs BusyLinux from out/rootfs.tar.gz. UEFI only; erases the whole disk.

    install.sh --disk /dev/nvme0n1 [--user NAME] [--hostname NAME]
               [--timezone Europe/Berlin] [--keymap de/de-latin1] [--yes]

Partition 1 is a 1 GiB ESP at /boot (kernel and Limine), partition 2 an ext4
root labelled BUSYLINUX_ROOT. --timezone needs tzdata and --keymap needs
kbd-bkeymaps on the system running this script. --user joins video, input,
audio, seat, wheel and rtkit.
EOT
}

DISK='' USER_NAME='' HOSTNAME=busylinux TIMEZONE='' KEYMAP='' YES=0 MNT=''
die()  { printf '\033[1;31merror: %s\033[0m\n' "$*" >&2; exit 1; }
log()  { printf '\n\033[1;32m==> %s\033[0m\n' "$*"; }

while [ $# -gt 0 ]; do
    case $1 in
        --disk)     DISK=$2; shift 2 ;;
        --user)     USER_NAME=$2; shift 2 ;;
        --hostname) HOSTNAME=$2; shift 2 ;;
        --timezone) TIMEZONE=$2; shift 2 ;;
        --keymap)   KEYMAP=$2; shift 2 ;;
        --yes|-y)   YES=1; shift ;;
        -h|--help)  usage; exit 0 ;;
        *)          die "unknown option: $1" ;;
    esac
done

[ "$(id -u)" = 0 ] || die "run this as root"
[ -b "$DISK" ] || die "--disk: '$DISK' is not a block device (try --help)"
case $HOSTNAME in ''|[.-]*|*[!A-Za-z0-9.-]*|*.) die "--hostname: letters, digits, - and . only" ;; esac
case $TIMEZONE in
    ''|UTC) TIMEZONE='' ;;
    /*|*..*|*[!A-Za-z0-9_+/-]*) die "--timezone: '$TIMEZONE' is not a zone name" ;;
    *) [ -f "/usr/share/zoneinfo/$TIMEZONE" ] || die "--timezone: no zone $TIMEZONE (apk add tzdata)" ;;
esac
case $KEYMAP in
    '') ;;
    */*/*|*..*|*[!A-Za-z0-9_/-]*|*/|/*) die "--keymap: expected LAYOUT/VARIANT" ;;
    */*) [ -f "/usr/share/bkeymaps/$KEYMAP.bmap.gz" ] ||
             die "--keymap: no keymap $KEYMAP (apk add kbd-bkeymaps)" ;;
    *) die "--keymap: expected LAYOUT/VARIANT" ;;
esac
TAR=''
for d in ./out "$(dirname "$0")/out"; do [ -f "$d/rootfs.tar.gz" ] && TAR=$d/rootfs.tar.gz && break; done
[ -n "$TAR" ] || die "no out/rootfs.tar.gz; run the build first"
for t in sgdisk mkfs.vfat mkfs.ext4; do
    command -v "$t" > /dev/null || die "$t is missing (apk add sgdisk dosfstools e2fsprogs)"
done

if [ "$YES" != 1 ]; then
    printf 'Everything on %s will be destroyed. Type the disk name to go on: ' "$DISK"
    read -r answer
    [ "$answer" = "$DISK" ] || die "not confirmed"
fi

log "Partitioning $DISK"
sgdisk --zap-all "$DISK" > /dev/null
sgdisk -n 1:0:+1G -t 1:ef00 -c 1:"EFI system" -n 2:0:0 -t 2:8300 -c 2:"BusyLinux root" \
    "$DISK" > /dev/null
case $DISK in *[0-9]) ESP=${DISK}p1 ROOT=${DISK}p2 ;; *) ESP=${DISK}1 ROOT=${DISK}2 ;; esac
i=0
while [ ! -b "$ESP" ] || [ ! -b "$ROOT" ]; do
    [ "$i" -lt 15 ] || die "$ESP and $ROOT never appeared; reboot and try again"
    partprobe "$DISK" 2>/dev/null || partx -u "$DISK" 2>/dev/null || blockdev --rereadpt "$DISK" 2>/dev/null || :
    mdev -s 2>/dev/null || udevadm settle 2>/dev/null || :
    sleep 1; i=$((i + 1))
done
mkfs.vfat -F 32 -n BUSYLINUX "$ESP" > /dev/null
mkfs.ext4 -q -F -L BUSYLINUX_ROOT "$ROOT"

log "Unpacking"
MNT=$(mktemp -d)
trap 'umount "$MNT/boot" "$MNT" 2>/dev/null; rmdir "$MNT"' EXIT
mount "$ROOT" "$MNT"
tar -xpf "$TAR" -C "$MNT" --numeric-owner
# FAT keeps no modes, so /boot is copied rather than unpacked onto the ESP.
mv "$MNT/boot" "$MNT/boot.new"
mkdir "$MNT/boot"
mount "$ESP" "$MNT/boot"
cp -R "$MNT/boot.new/." "$MNT/boot/"
rm -rf "$MNT/boot.new"

log "Configuring"
echo "$HOSTNAME" > "$MNT/etc/hostname"
printf '127.0.0.1\tlocalhost\n::1\t\tlocalhost\n127.0.1.1\t%s\n' "$HOSTNAME" > "$MNT/etc/hosts"
rm -f "$MNT/etc/localtime"
[ -z "$TIMEZONE" ] || cp "/usr/share/zoneinfo/$TIMEZONE" "$MNT/etc/localtime"
if [ -n "$KEYMAP" ]; then
    mkdir -p "$MNT/etc/keymap"
    cp "/usr/share/bkeymaps/$KEYMAP.bmap.gz" "$MNT/etc/keymap/"
fi
cat > "$MNT/etc/fstab" <<'EOF'
LABEL=BUSYLINUX_ROOT  /      ext4   rw,noatime                        0 1
LABEL=BUSYLINUX       /boot  vfat   rw,noatime,fmask=0077,dmask=0077  0 2
tmpfs                 /tmp   tmpfs  rw,nosuid,nodev,size=8G           0 0
EOF

mkdir -p "$MNT/boot/EFI/BOOT"
cp "$MNT/usr/share/limine/BOOTX64.EFI" "$MNT/boot/EFI/BOOT/"
entry() {
    printf '\n/%s\n    protocol: linux\n    path: boot():/%s\n    cmdline: %s\n' "$1" "$2" "$3"
    printf '    module_path: boot():/%s\n' amd-ucode.img initramfs.cpio.gz
}
{
    printf 'timeout: 3\nserial: yes\n'
    entry BusyLinux vmlinuz-busylinux "root=LABEL=BUSYLINUX_ROOT ro"
    entry "BusyLinux (serial console)" vmlinuz-busylinux \
        "root=LABEL=BUSYLINUX_ROOT ro console=tty0 console=ttyS0,115200"
    entry "Rescue shell" vmlinuz-busylinux rescue
} > "$MNT/boot/EFI/BOOT/limine.conf"
if [ -d /sys/firmware/efi/efivars ] && command -v efibootmgr > /dev/null; then
    efibootmgr -c -d "$DISK" -p 1 -l '\EFI\BOOT\BOOTX64.EFI' -L BusyLinux > /dev/null ||
        echo "no UEFI boot entry; the firmware finds \\EFI\\BOOT\\BOOTX64.EFI by itself"
fi

if [ -n "$USER_NAME" ]; then
    chroot "$MNT" adduser -D "$USER_NAME"
    for g in seat wheel rtkit; do chroot "$MNT" addgroup -S "$g" 2>/dev/null || :; done
    for g in video input audio seat wheel rtkit; do chroot "$MNT" addgroup "$USER_NAME" "$g"; done
fi
if [ -t 0 ]; then
    log "Passwords"
    chroot "$MNT" passwd root
    [ -z "$USER_NAME" ] || chroot "$MNT" passwd "$USER_NAME"
else
    echo "root has no password: run passwd after the first boot"
fi
sync
log "Done: turn Secure Boot off, then boot $DISK"
