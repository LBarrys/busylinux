#!/usr/bin/env bash
# Boots a build under QEMU and checks it over the serial console.
# Usage: tests/boot-test.sh [OUT_DIR]. The log goes to OUT_DIR/boot-test.log.
set -euo pipefail

OUT=${1:-out}
BOOT_TIMEOUT=${BOOT_TIMEOUT:-300}
for f in vmlinuz initramfs.cpio.gz disk.img; do
    [ -f "$OUT/$f" ] || { echo "boot-test: $OUT/$f is missing" >&2; exit 1; }
done

work=$(mktemp -d)
log=$OUT/boot-test.log
qpid='' rpid=''
cleanup() {
    [ -z "$qpid" ] || kill "$qpid" 2>/dev/null || :
    [ -z "$rpid" ] || kill "$rpid" 2>/dev/null || :
    rm -rf "$work"
}
trap cleanup EXIT

pass() { printf '\033[1;32m  ok   %s\033[0m\n' "$*"; }
fail() {
    printf '\033[1;31m  FAIL %s\033[0m\n' "$*" >&2
    printf '\n--- last 40 lines of %s ---\n' "$log" >&2
    console | tail -n 40 >&2
    exit 1
}

# The log without CRs and escapes (ash queries the cursor after each prompt).
console() { tr -d '\r' < "$log" | sed 's/\x1b\[[0-9;?]*[A-Za-z]//g'; }

wait_for() {
    local deadline=$((SECONDS + $2))
    while [ "$SECONDS" -lt "$deadline" ]; do
        console | grep -Eq -- "$1" && return 0
        [ -z "$qpid" ] || kill -0 "$qpid" 2>/dev/null || return 1
        sleep 1
    done
    return 1
}

send() { printf '%s\r' "$*" > "$work/serial.in"; }

# printf builds the marker, so the echoed command line cannot match it.
check() {
    local name=$1; shift
    send "$* && printf 'CHECK-%s\\n' $name"
    wait_for "CHECK-$name\$" "${CHECK_TIMEOUT:-90}" || fail "$name"
    pass "$name"
}

cp --sparse=always "$OUT/disk.img" "$work/disk.img"

# A checkout and a signing key, so update.sh can run offline in the guest.
repo=$(cd "$(dirname "$0")/.." && pwd)
openssl genrsa -out "$work/local.rsa" 2048 2>/dev/null
openssl rsa -in "$work/local.rsa" -pubout -out "$work/local.rsa.pub" 2>/dev/null
# A service that needs 3 s to stop, to show rcK waits for it.
cat > "$work/slowstop" <<'EOT'
#!/bin/sh
trap 'sleep 3; echo "slowstop: stopped cleanly" > /dev/console; exit 0' TERM
while :; do sleep 1; done
EOT
chmod 755 "$work/slowstop"
{
    echo "mkdir /etc/service/slowstop"
    echo "write $work/slowstop /etc/service/slowstop/run"
    echo "mkdir /root/keys"
    echo "write $work/local.rsa /root/keys/local.rsa"
    echo "write $work/local.rsa.pub /etc/apk/keys/local.rsa.pub"
    for d in busylinux busylinux/pkgs busylinux/pkgs/busylinux-init \
             busylinux/pkgs/busylinux-init/files; do
        echo "mkdir /root/$d"
    done
    echo "write $repo/update.sh /root/busylinux/update.sh"
    for f in "$repo"/pkgs/busylinux-init/* "$repo"/pkgs/busylinux-init/files/*; do
        [ -f "$f" ] && echo "write $f /root/busylinux/${f#"$repo"/}"
    done
} > "$work/checkout.debugfs"
debugfs -w -f "$work/checkout.debugfs" "$work/disk.img" > /dev/null 2>&1
mkfifo "$work/serial.in" "$work/serial.out" "$work/mon.in" "$work/mon.out"
: > "$log"

accel=tcg
if [ -w /dev/kvm ]; then accel=kvm; fi
echo "boot-test: booting $OUT under QEMU ($accel)"

qemu-system-x86_64 -nodefaults -display none -no-reboot \
    -machine q35,accel=$accel -m 1G -smp 2 \
    -kernel "$OUT/vmlinuz" -initrd "$OUT/initramfs.cpio.gz" \
    -append "root=LABEL=BUSYLINUX_ROOT ro console=ttyS0,115200" \
    -drive file="$work/disk.img",format=raw,if=virtio \
    -nic user,model=virtio-net-pci \
    -device qemu-xhci,id=xhci -audiodev none,id=snd0 \
    -chardev pipe,id=ser,path="$work/serial" -serial chardev:ser \
    -chardev pipe,id=mon,path="$work/mon" -mon chardev=mon,mode=readline &
qpid=$!
cat "$work/serial.out" >> "$log" &
rpid=$!
cat "$work/mon.out" > /dev/null &

wait_for 'login: *$' "$BOOT_TIMEOUT" || fail "login prompt"
pass "login prompt"
send root
wait_for '# *$' 30 || fail "root shell"
pass "root shell"
send 'stty -echo 2>/dev/null; export PS1="# "'
sleep 1

check firewall   'nft list chain inet filter input | grep -q "policy drop"'
check forward    'nft list chain inet filter forward | grep -q "policy drop"'
check dhcp       'for i in $(seq 60); do ip -4 addr show | grep -q "inet 10\.0\.2\." && break; sleep 1; done; ip -4 addr show | grep -q "inet 10\.0\.2\."'
check services   'for i in $(seq 10); do [ "$(for s in syslogd klogd crond acpid dhcp slowstop; do sv status /etc/service/$s; done | grep -c "^run:")" = 6 ] && break; sleep 1; done; [ "$i" -lt 10 ]'
check cgroup2    'grep -q "^cgroup2 /sys/fs/cgroup " /proc/mounts'
check devfd      '[ -L /dev/fd ] && [ "$(echo fd-ok | cat /dev/stdin)" = fd-ok ] && [ -e /dev/fd/0 ]'
check tmp        'grep -q "^tmpfs /tmp tmpfs rw,nosuid,nodev" /proc/mounts'
check ntsync     '[ "$(stat -c %a /dev/ntsync)" = 666 ]'
check uinput     '[ "$(stat -c %G:%a /dev/uinput)" = input:660 ]'
check sysrq      '[ "$(cat /proc/sys/kernel/sysrq)" = 244 ]'
check mglru      '[ "$(cat /sys/kernel/mm/lru_gen/enabled)" != 0x0000 ]'

check fail-closed 'nft flush ruleset && sv restart /etc/service/dhcp >/dev/null; sleep 3; grep -q "no firewall ruleset is loaded" /var/log/messages'
check reload     'nft -f /etc/nftables.conf && sv restart /etc/service/dhcp >/dev/null'
# A bridge sorts before eth0; DHCP must still go to the network card.
check dhcp-bridge 'ip link add br-test type bridge && ip link set br-test up && sv restart /etc/service/dhcp >/dev/null; sleep 2; pgrep -f "udhcpc -f -i eth0" >/dev/null && ! pgrep -f "udhcpc -f -i br-test" >/dev/null && ip link del br-test'
check libudev-zero 'apk add --no-network -q libudev-zero >/dev/null 2>&1 && apk info -e libudev-zero >/dev/null && grep -q SOUND_INITIALIZED /usr/lib/libudev.so.1'
# mdev through libudev-zero's relay: a hot-plugged sound card gets module and node.
check mdev-relay 'kill $(pidof mdev) && /usr/libexec/libudev-zero-mdev && pidof libudev-zero-mdev >/dev/null'
echo 'device_add usb-audio,id=usbsnd,audiodev=snd0,bus=xhci.0' > "$work/mon.in"
check hotplug    'for i in $(seq 30); do [ -c /dev/snd/controlC0 ] && break; sleep 1; done; grep -q "^snd_usb_audio " /proc/modules && [ "$(stat -c %G:%a /dev/snd/controlC0)" = audio:660 ]'
check update     'b=$(apk list --installed busylinux-init) && sh /root/busylinux/update.sh --yes busylinux-init >/tmp/update.log 2>&1 && a=$(apk list --installed busylinux-init) && [ "$a" != "$b" ] && [ -x /etc/init.d/rcS ] || { tail -n 20 /tmp/update.log; false; }'

echo system_powerdown > "$work/mon.in"
deadline=$((SECONDS + 90))
while kill -0 "$qpid" 2>/dev/null && [ "$SECONDS" -lt "$deadline" ]; do sleep 1; done
kill -0 "$qpid" 2>/dev/null && fail "power button: still running after 90 s"
wait "$qpid" || :
qpid=''
console | grep -q 'Shutting down' || fail "power button: rcK never ran"
pass "power button"
console | grep -q 'slowstop: stopped cleanly' || fail "shutdown: services were not given time to stop"
pass "services stopped"
if dumpe2fs -h "$work/disk.img" 2>/dev/null | grep -q '^Filesystem features:.*needs_recovery'; then
    fail "shutdown: the root file system was left dirty"
fi
pass "clean root file system"

echo "boot-test: all checks passed"
