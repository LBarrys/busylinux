#!/usr/bin/env bash
# Boots a finished build under QEMU and checks it over the serial console:
# login, the firewall, the fail-closed network, the services, device modes,
# and a clean shutdown when the ACPI power button is pressed.
#
#     tests/boot-test.sh [OUT_DIR]      (default: out)
#
# Needs qemu-system-x86_64; uses KVM when /dev/kvm is writable. Exits non-zero
# on the first failed check and leaves the console log in OUT_DIR/boot-test.log.
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

# The console log without carriage returns and terminal escapes (ash asks the
# terminal for the cursor position after every prompt).
console() { tr -d '\r' < "$log" | sed 's/\x1b\[[0-9;?]*[A-Za-z]//g'; }

# Wait until the console log shows $1 (an extended regex) or $2 seconds pass.
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

# Run a command in the guest; it must print CHECK-$1 when the check passes.
# The marker is assembled by printf, so the echoed command line never
# contains it and cannot satisfy the match by itself.
check() {
    local name=$1; shift
    send "$* && printf 'CHECK-%s\\n' $name"
    wait_for "CHECK-$name\$" "${CHECK_TIMEOUT:-90}" || fail "$name"
    pass "$name"
}

cp --sparse=always "$OUT/disk.img" "$work/disk.img"
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
check services   '[ "$(for s in syslogd klogd crond acpid dhcp; do sv status /etc/service/$s; done | grep -c "^run:")" = 5 ]'
check cgroup2    'grep -q "^cgroup2 /sys/fs/cgroup " /proc/mounts'
check ntsync     '[ "$(stat -c %a /dev/ntsync)" = 666 ]'
check uinput     '[ "$(stat -c %G:%a /dev/uinput)" = input:660 ]'
check hwclock    'grep -q "hwclock -u -w" /etc/crontabs/root'
check sysrq      '[ "$(cat /proc/sys/kernel/sysrq)" = 244 ]'
check motd       '[ ! -s /etc/motd ]'

# Fail closed: with the ruleset gone, the dhcp service must refuse to start.
check fail-closed 'nft flush ruleset && sv restart /etc/service/dhcp >/dev/null; sleep 3; grep -q "no firewall ruleset is loaded" /var/log/messages'
check reload     'nft -f /etc/nftables.conf && sv restart /etc/service/dhcp >/dev/null'

# The power button: QEMU raises the ACPI event, acpid runs poweroff, init runs
# rcK, and the machine turns itself off.
echo system_powerdown > "$work/mon.in"
deadline=$((SECONDS + 90))
while kill -0 "$qpid" 2>/dev/null && [ "$SECONDS" -lt "$deadline" ]; do sleep 1; done
kill -0 "$qpid" 2>/dev/null && fail "power button: still running after 90 s"
wait "$qpid" || :
qpid=''
console | grep -q 'Shutting down' || fail "power button: rcK never ran"
pass "power button"

echo "boot-test: all checks passed"
