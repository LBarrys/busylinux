#!/bin/sh
set -eu

usage() {
    cat <<'EOT'
Install BusyLinux onto a disk. UEFI only.

    install.sh --disk /dev/nvme0n1

Writes a GPT with two partitions; nothing else on the disk survives:

    1   1 GiB    EFI system  FAT32, mounted at /boot, holds the kernel and Limine
    2   rest     root        ext4, labelled BUSYLINUX_ROOT

Options:
    --disk DEV          the disk to install to (required)
    --dir DIR           where out/ is (default: ./out, then the script's dir)
    --esp-size SIZE     EFI system partition size (default 1G)
    --root-size SIZE    root partition size (default: the rest of the disk)
    --hostname NAME     default busylinux
    --user NAME         create this user in video/input/audio/seat, set a password
    --no-nvram          do not add a UEFI boot entry
    --yes               do not ask for confirmation
EOT
}

DISK='' DIR='' ESP_SIZE=1G ROOT_SIZE='' HOSTNAME=busylinux USER_NAME=''
NVRAM=1 ASSUME_YES=0
ROOT_LABEL=BUSYLINUX_ROOT
ESP_LABEL=BUSYLINUX
MNT=''

die()  { printf '\033[1;31merror: %s\033[0m\n' "$*" >&2; exit 1; }
log()  { printf '\n\033[1;32m==> %s\033[0m\n' "$*"; }
info() { printf '\033[1;34m  > %s\033[0m\n' "$*"; }

cleanup() {
    [ -n "$MNT" ] || return 0
    umount "$MNT/boot" 2>/dev/null || :
    umount "$MNT" 2>/dev/null || :
    rmdir "$MNT" 2>/dev/null || :
}
trap cleanup EXIT INT TERM

while [ $# -gt 0 ]; do
    case $1 in
        --disk)      DISK=$2; shift 2 ;;
        --dir)       DIR=$2; shift 2 ;;
        --esp-size)  ESP_SIZE=$2; shift 2 ;;
        --root-size) ROOT_SIZE=$2; shift 2 ;;
        --hostname)  HOSTNAME=$2; shift 2 ;;
        --user)      USER_NAME=$2; shift 2 ;;
        --no-nvram)  NVRAM=0; shift ;;
        --yes|-y)    ASSUME_YES=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        *)           die "unknown option: $1" ;;
    esac
done

[ "$(id -u)" = 0 ] || die "run this as root"
[ -n "$DISK" ]     || die "no --disk given (try --help)"
[ -b "$DISK" ]     || die "$DISK is not a block device"

if [ -z "$DIR" ]; then
    for d in ./out "$(dirname "$0")/out" "$(dirname "$0")"; do
        if [ -f "$d/rootfs.tar.gz" ]; then DIR=$d; break; fi
    done
fi
[ -n "$DIR" ] || die "cannot find rootfs.tar.gz; pass --dir"
for f in rootfs.tar.gz initramfs.cpio.gz; do
    [ -f "$DIR/$f" ] || die "$DIR/$f is missing -- run the build first"
done

need() { command -v "$1" > /dev/null 2>&1 || die "$1 is missing${2:+ (install $2)}"; }
need sgdisk   "sgdisk (Alpine: apk add sgdisk)"
need mkfs.vfat dosfstools
need mkfs.ext4 e2fsprogs
need tar

if [ ! -d /sys/firmware/efi/efivars ]; then
    NVRAM=0
    info "not booted via UEFI: installing the removable path only"
fi

log "About to erase $DISK"
sgdisk --print "$DISK" 2>/dev/null | tail -n +5 || :
printf '\n'
if [ "$ASSUME_YES" != 1 ]; then
    printf 'Everything on %s will be destroyed. Type the disk name to go on: ' "$DISK"
    read -r answer
    [ "$answer" = "$DISK" ] || die "not confirmed"
fi

log "Partitioning $DISK"
wipefs -a "$DISK" > /dev/null 2>&1 || :
sgdisk --zap-all "$DISK" > /dev/null
root_end=0
if [ -n "$ROOT_SIZE" ]; then root_end="+$ROOT_SIZE"; fi
sgdisk \
    -n "1:0:+$ESP_SIZE" -t 1:ef00 -c 1:"EFI system" \
    -n "2:0:$root_end"  -t 2:8300 -c 2:"BusyLinux root" \
    "$DISK" > /dev/null

part() { case $DISK in *[0-9]) printf '%sp%s\n' "$DISK" "$1" ;;
                       *)      printf '%s%s\n'  "$DISK" "$1" ;; esac; }
ESP=$(part 1) ROOT=$(part 2)

reread() {
    partprobe "$DISK"            > /dev/null 2>&1 && return 0
    partx -u "$DISK"             > /dev/null 2>&1 && return 0
    losetup -c "$DISK"           > /dev/null 2>&1 && return 0
    blockdev --rereadpt "$DISK"  > /dev/null 2>&1 && return 0
    return 0
}

settle() {
    if command -v udevadm > /dev/null 2>&1; then
        udevadm settle -t 10 > /dev/null 2>&1 || :
    elif command -v mdev > /dev/null 2>&1; then
        mdev -s > /dev/null 2>&1 || :
    fi
}

wait_parts() {
    tries=0
    while [ ! -b "$ESP" ] || [ ! -b "$ROOT" ]; do
        if [ "$tries" -ge 15 ]; then
            die "$ESP and $ROOT never appeared. Reboot and run this again, or
install parted or util-linux so the partition table can be re-read."
        fi
        tries=$((tries + 1)); sleep 1; reread; settle
    done
}

reread
settle
wait_parts

log "Creating filesystems"
mkfs.vfat -F 32 -n "$ESP_LABEL" "$ESP" > /dev/null
mkfs.ext4 -q -F -L "$ROOT_LABEL" "$ROOT"
sync
sleep 2
settle
wait_parts
info "$ESP  vfat  $ESP_LABEL"
info "$ROOT  ext4  $ROOT_LABEL"

log "Unpacking the root filesystem"
MNT=$(mktemp -d)
mount -t ext4 "$ROOT" "$MNT"
mkdir -p "$MNT/boot"
mount -t vfat "$ESP" "$MNT/boot"
tar -xpf "$DIR/rootfs.tar.gz" -C "$MNT" --numeric-owner
cp "$DIR/initramfs.cpio.gz" "$MNT/boot/"
if [ -f "$DIR/amd-ucode.img" ]; then cp "$DIR/amd-ucode.img" "$MNT/boot/"; fi
info "$(du -sh "$MNT" | cut -f1) on $ROOT, $(du -sh "$MNT/boot" | cut -f1) on $ESP"

log "Configuring"
printf '%s\n' "$HOSTNAME" > "$MNT/etc/hostname"
cat > "$MNT/etc/fstab" <<EOF
LABEL=$ROOT_LABEL  /             ext4     rw,relatime                       0      1
LABEL=$ESP_LABEL       /boot         vfat     rw,noatime,fmask=0077,dmask=0077  0      2
/dev/cdrom              /media/cdrom  iso9660  noauto,ro                         0      0
EOF

log "Installing Limine"
LIMINE_EFI=$MNT/usr/share/limine/BOOTX64.EFI
[ -f "$LIMINE_EFI" ] ||
    die "$LIMINE_EFI is missing -- the image was built without limine-efi-x86_64"

kernel=$(cd "$MNT/boot" && ls vmlinuz-* 2>/dev/null | head -1)
[ -n "$kernel" ] || die "no kernel in $MNT/boot -- rootfs.tar.gz looks wrong"

mkdir -p "$MNT/boot/EFI/BOOT"
cp "$LIMINE_EFI" "$MNT/boot/EFI/BOOT/BOOTX64.EFI"

ucode=''
if [ -f "$MNT/boot/amd-ucode.img" ]; then
    ucode="    module_path: boot():/amd-ucode.img
"
fi
cat > "$MNT/boot/EFI/BOOT/limine.conf" <<EOF
timeout: 3
default_entry: 1
serial: yes

/BusyLinux
    protocol: linux
    path: boot():/$kernel
    cmdline: root=LABEL=$ROOT_LABEL rw
$ucode    module_path: boot():/initramfs.cpio.gz

/BusyLinux (serial console on ttyS0)
    protocol: linux
    path: boot():/$kernel
    cmdline: root=LABEL=$ROOT_LABEL rw console=tty0 console=ttyS0,115200
$ucode    module_path: boot():/initramfs.cpio.gz

/Rescue shell (initramfs only)
    protocol: linux
    path: boot():/$kernel
    cmdline: rescue
$ucode    module_path: boot():/initramfs.cpio.gz
EOF
info "$(du -h "$MNT/boot/EFI/BOOT/BOOTX64.EFI" | cut -f1) BOOTX64.EFI + limine.conf"

if [ "$NVRAM" = 1 ] && command -v efibootmgr > /dev/null 2>&1; then
    efibootmgr 2>/dev/null | sed -n 's/^Boot\([0-9A-Fa-f]\{4\}\)\*\? BusyLinux$/\1/p' |
    while read -r num; do
        efibootmgr -b "$num" -B > /dev/null 2>&1 || :
    done
    if efibootmgr --create --disk "$DISK" --part 1 \
         --loader '\EFI\BOOT\BOOTX64.EFI' --label BusyLinux > /dev/null 2>&1; then
        info "added the UEFI boot entry 'BusyLinux'"
    else
        info "could not add a UEFI boot entry; the removable path will do"
    fi
fi

if [ -n "$USER_NAME" ]; then
    log "Creating $USER_NAME"
    chroot "$MNT" /bin/busybox adduser -D "$USER_NAME"
    chroot "$MNT" /bin/busybox addgroup -S seat 2>/dev/null || :
    for g in video input audio seat wheel; do
        chroot "$MNT" /bin/busybox addgroup "$USER_NAME" "$g" 2>/dev/null || :
    done
    if [ -t 0 ]; then chroot "$MNT" /bin/busybox passwd "$USER_NAME"; fi
fi

if [ -t 0 ]; then
    log "Setting the root password"
    chroot "$MNT" /bin/busybox passwd root ||
        info "root still has no password; set one after the first boot"
elif [ -n "$USER_NAME" ]; then
    chroot "$MNT" /bin/busybox passwd -l root > /dev/null 2>&1 || :
    sed -i "s|^$USER_NAME:[^:]*:|$USER_NAME::|" "$MNT/etc/shadow"
    info "root is locked and $USER_NAME has an EMPTY password."
    info "Log in as $USER_NAME on tty1 and run 'passwd' before anything else."
else
    info "WARNING: root has an EMPTY password and getty runs on tty1-3 and ttyS0."
    info "WARNING: anyone at the keyboard is root. Run 'passwd root' on first boot."
fi

sync
log "Done"
cat <<EOF

  Installed to $DISK
    $ESP   /boot   FAT32, $kernel + Limine
    $ROOT   /       ext4, LABEL=$ROOT_LABEL

  Reboot, and in the firmware setup:
    - turn Secure Boot OFF (this kernel is not signed)
    - leave CSM off and boot in UEFI mode
    - EXPO stays on; the kernel needs nothing for it

  First things to do on the new system:
    apk update
    apk add linux-firmware-amdgpu linux-firmware-mediatek

EOF
