#!/bin/sh
# Installs out/rootfs.tar.gz with install.sh onto a disk image, boots it under
# QEMU with UEFI firmware and Limine, and checks it over the serial console.
# Run as root; needs qemu-system-x86_64, OVMF, sgdisk and dosfstools.
set -eu

top=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
log=$top/out/boot-test.log
qpid='' loop=''
cleanup() {
    [ -z "$qpid" ] || kill "$qpid" 2>/dev/null || :
    umount "$work/mnt/boot" "$work/mnt" 2>/dev/null || :
    [ -z "$loop" ] || losetup -d "$loop" 2>/dev/null || :
    rm -rf "$work"
}
trap cleanup EXIT

pass() { printf '\033[1;32m  ok   %s\033[0m\n' "$*"; }
fail() {
    printf '\033[1;31m  FAIL %s\033[0m\n' "$*" >&2
    console | tail -n 40 >&2
    exit 1
}
# The log without CRs and escapes (ash queries the cursor after each prompt).
console() { tr -d '\r' < "$log" | sed 's/\x1b\[[0-9;?]*[A-Za-z]//g'; }
wait_for() {
    i=0
    while [ "$i" -lt "$2" ]; do
        console | grep -Eq -- "$1" && return 0
        kill -0 "$qpid" 2>/dev/null || return 1
        sleep 1; i=$((i + 1))
    done
    return 1
}
send() { printf '%s\r' "$*" > "$work/serial.in"; }
# printf builds the marker, so the echoed command line cannot match it.
check() {
    name=$1; shift
    send "$* && printf 'CHECK-%s\\n' $name"
    wait_for "CHECK-$name\$" 90 || fail "$name"
    pass "$name"
}

truncate -s 4G "$work/disk.img"
loop=$(losetup -fP --show "$work/disk.img")
(cd "$top" && tools/install.sh --disk "$loop" --yes) > "$work/install.log" 2>&1 ||
    { cat "$work/install.log"; exit 1; }
pass "install.sh"

# Boot the serial entry with a login on ttyS0; add a checkout for update.sh
# and a service that needs 3 s to stop, to show rcK waits for it.
mkdir "$work/mnt"
mount "${loop}p2" "$work/mnt"
mount "${loop}p1" "$work/mnt/boot"
sed -i 's/^timeout: 3$/timeout: 1\ndefault_entry: 2/' "$work/mnt/boot/EFI/BOOT/limine.conf"
echo 'ttyS0::respawn:/sbin/getty -L 115200 ttyS0 vt100' >> "$work/mnt/etc/inittab"
echo 'echo ran > /run/rc-local-ran' >> "$work/mnt/etc/rc.local"
mkdir -p "$work/mnt/root/busylinux/pkgs" "$work/mnt/root/busylinux/tools" "$work/mnt/root/keys" "$work/mnt/etc/service/slowstop"
cp "$top/tools/update.sh" "$top/tools/lib.sh" "$work/mnt/root/busylinux/tools/"
cp -R "$top/pkgs/busylinux-init" "$work/mnt/root/busylinux/pkgs/"
openssl genrsa -out "$work/mnt/root/keys/local.rsa" 2048 2>/dev/null
openssl rsa -in "$work/mnt/root/keys/local.rsa" -pubout \
    -out "$work/mnt/etc/apk/keys/local.rsa.pub" 2>/dev/null
cat > "$work/mnt/etc/service/slowstop/run" <<'EOF'
#!/bin/sh
trap 'sleep 3; echo "slowstop: stopped cleanly" > /dev/console; exit 0' TERM
while :; do sleep 1; done
EOF
chmod 755 "$work/mnt/etc/service/slowstop/run"
umount "$work/mnt/boot" "$work/mnt"
losetup -d "$loop"
loop=''

ovmf=''
for f in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd; do
    [ -f "$f" ] && [ -f "${f%CODE*}VARS${f#*CODE}" ] && ovmf=$f && break
done
[ -n "$ovmf" ] || { echo "boot-test: no OVMF firmware" >&2; exit 1; }
cp "${ovmf%CODE*}VARS${ovmf#*CODE}" "$work/vars.fd"
accel=tcg
[ -w /dev/kvm ] && accel=kvm
mkfifo "$work/serial.in" "$work/serial.out" "$work/mon.in" "$work/mon.out"
: > "$log"
qemu-system-x86_64 -nodefaults -display none -no-reboot \
    -machine q35,accel=$accel -m 1G -smp 2 \
    -drive if=pflash,format=raw,readonly=on,file="$ovmf" \
    -drive if=pflash,format=raw,file="$work/vars.fd" \
    -drive file="$work/disk.img",format=raw,if=virtio \
    -nic user,model=virtio-net-pci \
    -device qemu-xhci,id=xhci -audiodev none,id=snd0 \
    -chardev pipe,id=ser,path="$work/serial" -serial chardev:ser \
    -chardev pipe,id=mon,path="$work/mon" -mon chardev=mon,mode=readline &
qpid=$!
cat "$work/serial.out" >> "$log" &
cat "$work/mon.out" > /dev/null &

wait_for 'login: *$' 300 || fail "login prompt"
pass "UEFI, Limine and login prompt"
# The kernel mounts the root by partition name; rcS must still find and check it.
console | grep -q '^Checking /dev/vda2$' || fail "rcS did not check the root file system"
pass "root found and checked"
send root
wait_for '# *$' 30 || fail "root shell"
send 'stty -echo 2>/dev/null; export PS1="# "'
sleep 1

check firewall    'nft list chain inet filter input | grep -q "policy drop"'
check forward     'nft list chain inet filter forward | grep -q "policy drop"'
check dhcp        'for i in $(seq 60); do ip -4 addr show | grep -q "inet 10\.0\.2\." && break; sleep 1; done; ip -4 addr show | grep -q "inet 10\.0\.2\."'
check services    'for i in $(seq 10); do [ "$(for s in syslogd klogd crond acpid dhcp slowstop; do sv status /etc/service/$s; done | grep -c "^run:")" = 6 ] && break; sleep 1; done; [ "$i" -lt 10 ]'
check esp         'grep -q " /boot vfat " /proc/mounts && [ -f /boot/vmlinuz-busylinux ]'
check cgroup2     'grep -q "^cgroup2 /sys/fs/cgroup " /proc/mounts'
check devfd       '[ -L /dev/fd ] && [ "$(echo fd-ok | cat /dev/stdin)" = fd-ok ] && [ -e /dev/fd/0 ]'
check tmp         'grep -q "^tmpfs /tmp tmpfs rw,nosuid,nodev" /proc/mounts'
check ntsync      '[ "$(stat -c %a /dev/ntsync)" = 666 ]'
check uinput      '[ "$(stat -c %G:%a /dev/uinput)" = input:660 ]'
check sysrq       '[ "$(cat /proc/sys/kernel/sysrq)" = 244 ]'
check no-gnu      '! apk info -e gmp >/dev/null && ! apk info -e readline >/dev/null && nft -V | grep -q "json:.*yes"'
check syslog      'grep -q "udhcpc.*lease" /var/log/messages && ! pgrep -f "logger -t dhcp" >/dev/null'
check zram        'grep -q "^/dev/zram0 " /proc/swaps && [ "$(cat /proc/sys/vm/swappiness)" = 180 ]'
check rtprio      'grep -q "^Max realtime priority *95 *95 " /proc/$(pgrep -f "getty 38400 tty1" | head -n1)/limits'
check nofile      'grep -q "^Max open files *1024 *524288 " /proc/$(pgrep -f "getty 38400 tty1" | head -n1)/limits'
check map-count   '[ "$(cat /proc/sys/vm/max_map_count)" = 1048576 ]'
check rc-local    '[ "$(cat /run/rc-local-ran)" = ran ]'
check mimalloc    'grep -q libmimalloc /proc/self/maps && grep -q libmimalloc /proc/$(pidof syslogd)/maps'
check io-uring    '[ "$(cat /proc/sys/kernel/io_uring_disabled)" = 2 ]'
check mmap-min    '[ "$(cat /proc/sys/vm/mmap_min_addr)" = 65536 ]'
check ipv6-privacy '[ "$(cat /proc/sys/net/ipv6/conf/eth0/addr_gen_mode)" = 3 ] && [ "$(cat /proc/sys/net/ipv6/conf/eth0/use_tempaddr)" = 2 ]'
check landlock    'dmesg | grep -q "landlock: Up and running"'
check mglru       '[ "$(cat /sys/kernel/mm/lru_gen/enabled)" != 0x0000 ]'
check pinned      'grep -q "^v3 @busylinux " /etc/apk/repositories && grep -qx "busylinux-init@busylinux" /etc/apk/world'
check fail-closed 'nft flush ruleset && sv restart /etc/service/dhcp >/dev/null; sleep 3; grep -q "no firewall ruleset is loaded" /var/log/messages'
check reload      'nft -f /etc/nftables.conf && sv restart /etc/service/dhcp >/dev/null'
# A bridge sorts before eth0; DHCP must still go to the network card.
check dhcp-bridge 'ip link add br-test type bridge && ip link set br-test up && sv restart /etc/service/dhcp >/dev/null; sleep 2; pgrep -f "udhcpc .*-i eth0" >/dev/null && ! pgrep -f "udhcpc .*-i br-test" >/dev/null && ip link del br-test'
check libudev-zero 'apk add -q libudev-zero@busylinux >/dev/null 2>&1 && grep -q SOUND_INITIALIZED /usr/lib/libudev.so.1'
# mdevd: a hot-plugged sound card gets module and node.
check mdevd       'pidof mdevd >/dev/null'
echo 'device_add usb-audio,id=usbsnd,audiodev=snd0,bus=xhci.0' > "$work/mon.in"
check hotplug     'for i in $(seq 30); do [ -c /dev/snd/controlC0 ] && break; sleep 1; done; grep -q "^snd_usb_audio " /proc/modules && [ "$(stat -c %G:%a /dev/snd/controlC0)" = audio:660 ]'
check update      'b=$(apk list -I busylinux-init) && /root/busylinux/tools/update.sh --yes busylinux-init >/tmp/update.log 2>&1 && a=$(apk list -I busylinux-init) && [ "$a" != "$b" ] || { tail -n 20 /tmp/update.log; false; }'

# Packages built here must bring their libraries: each program must load.
send 'ok=0; apk add -q dwl@busylinux gammastep@busylinux pipemixer@busylinux >/tmp/desktop.log 2>&1 && ok=1; for b in dwl gammastep pipemixer; do if ldd /usr/bin/$b 2>&1 | grep -qi error; then echo "$b: libraries missing"; ok=0; fi; done; [ "$ok" = 1 ] && printf "CHECK-%s\\n" desktop || tail -n 5 /tmp/desktop.log'
wait_for 'CHECK-desktop$' 600 || fail "desktop packages load"
pass "desktop packages load"

echo system_powerdown > "$work/mon.in"
i=0
while kill -0 "$qpid" 2>/dev/null && [ "$i" -lt 90 ]; do sleep 1; i=$((i + 1)); done
kill -0 "$qpid" 2>/dev/null && fail "power button: still running after 90 s"
qpid=''
console | grep -q 'Shutting down' || fail "power button: rcK never ran"
pass "power button"
console | grep -q 'slowstop: stopped cleanly' || fail "shutdown: services were not given time to stop"
pass "services stopped"
loop=$(losetup -fP --show "$work/disk.img")
if dumpe2fs -h "${loop}p2" 2>/dev/null | grep -q '^Filesystem features:.*needs_recovery'; then
    fail "shutdown: the root file system was left dirty"
fi
pass "clean root file system"
if console | grep -Eq '(crond|udhcpc|dbus-daemon)\[[0-9]+\]:'; then
    fail "services logged to the console instead of syslog"
fi
pass "quiet console"
echo "boot-test: all checks passed"
