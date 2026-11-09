# Network and firewall

## Wired, by DHCP

The `dhcp` service runs BusyBox `udhcpc` on the **first real network card**:
the first entry in `/sys/class/net` that has a device behind it and is
Ethernet. That skips `lo`, Docker and Podman bridges, veths and WireGuard. It
writes `/etc/resolv.conf` and the default route, and renews the lease by itself.

Before it brings the card up it sets two IPv6 privacy options:

- `addr_gen_mode=3`: the stable address comes from random bits, not the MAC.
- `use_tempaddr=2`: outgoing connections prefer temporary addresses, which change daily.

It also sends no vendor class (`-V ''`). By default udhcpc sends
`udhcp VERSION`, telling the network which OS asks.

`udhcpc` logs to syslog: `grep udhcpc /var/log/messages`.

### A static address

Stop the DHCP service for good and set the address in `/etc/rc.local`, which
runs after the firewall is loaded:

```sh
doas touch /etc/service/dhcp/down
doas sv down /etc/service/dhcp
```

```sh
# /etc/rc.local
ip link set eth0 up
ip addr add 192.168.1.20/24 dev eth0
ip route add default via 192.168.1.1
echo 'nameserver 192.168.1.1' > /etc/resolv.conf
```

### Wi-Fi

Not in the kernel (no `WLAN`). Adding it means `CONFIG_WLAN`, `CFG80211`,
`MAC80211`, the card's driver and its firmware, then taking `WLAN` off the
"crept back" list in `pkgs/linux-busylinux/build`, plus `iwd` or
`wpa_supplicant` and a service for it.

## The firewall

`/etc/nftables.conf`, loaded by `rcS` at boot:

- **input**: drops everything unasked-for. Allowed: loopback, replies to your
  own connections, ICMP and ICMPv6 (ping, path MTU, IPv6 neighbour discovery),
  DHCP and DHCPv6 replies, and DNS from Podman containers to `aardvark-dns`.
  Dropped packets are counted (`nft list chain inet filter input`).
- **forward**: drops by default. Allowed: replies, traffic out of Docker
  (`docker0`, `br-*`) and Podman (`podman*`) bridges, and traffic to ports
  containers publish (`ct status dnat`).
- **output**: everything allowed.

To open a port, say SSH, add to the `input` chain before `counter`:

```
tcp dport 22 accept
```

and reload with `doas nft -f /etc/nftables.conf`. apk keeps your edited file.

### Failing closed

If `/etc/nftables.conf` exists but no ruleset is loaded (a typo, a missing
`nft`), the `dhcp` service will not bring the network up. It logs
`no firewall ruleset is loaded` and tries again every 30 seconds. So a broken
ruleset means no network, never an open machine. Fix the file, then
`doas nft -f /etc/nftables.conf`; the network follows by itself.

To run with no firewall at all, on purpose, move `/etc/nftables.conf` away.

### nftables itself

`pkgs/nftables` is nftables built with mini-gmp and without readline, so no
GNU GMP or Readline is installed. The one loss is the interactive `nft -i`.
JSON support stays.

## Time

`ntpd` (BusyBox) syncs from `pool.ntp.org` and `time.cloudflare.com` once
`/etc/resolv.conf` has a server. `rcK` writes the time to the hardware clock
at shutdown.

## VPN

See [Cloudflare WARP](Cloudflare-WARP.md). Any WireGuard config works the same
way with `wg` and `ip`: the kernel has WireGuard built in.
