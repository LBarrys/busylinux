# Cloudflare WARP

Cloudflare's own WARP client needs glibc and policy routing (`ip rule`
with fwmarks), and the kernel leaves policy routing out. But WARP is plain
WireGuard, which the kernel has. So BusyLinux uses:

- **wgcf** (`pkgs/wgcf`, newer than Alpine's) to register an account and
  write a WireGuard profile, once;
- **`tools/warp`**, a short `sh` script, to bring the tunnel up and down
  with `wg` and BusyBox `ip` alone. No bash `wg-quick`, no daemon.

## Set up

```sh
tools/warp-setup          # through doas
doas warp on
wget -qO- https://www.cloudflare.com/cdn-cgi/trace | grep warp    # warp=on
doas warp off
```

`warp-setup`:

1. installs this repository's `wgcf` for the run only (building it with
   `update.sh` if the local repository lacks it) and `wireguard-tools-wg`
   (`wg` alone, without `wg-quick` and bash);
2. registers a free account (`/etc/wireguard/wgcf-account.toml`) unless one exists;
3. writes `/etc/wireguard/wgcf-profile.conf`, mode 600;
4. installs `tools/warp` as `/usr/sbin/warp`;
5. removes `wgcf` again.

### WARP+

```sh
tools/warp-setup YOUR-LICENSE-KEY
```

This binds the key to the account and writes the profile anew. Run it while
WARP is off.

## What `warp on` does

1. Creates a WireGuard interface `wgcf` from the profile, with MTU 1280
   (safe on any path, including PPPoE and mobile), and adds the profile's
   IPv4 and IPv6 addresses.
2. Routes the WARP endpoint itself through your current default gateway, so
   the tunnel's own packets do not loop into the tunnel.
3. Routes `0.0.0.0/1` and `128.0.0.0/1` (and `::/1`, `8000::/1` for IPv6) into
   the tunnel. Two halves are more specific than the default route, so they
   win, and the default route stays as it is. When `udhcpc` renews the lease and
   replaces the default route, the tunnel is untouched.
4. Saves `/etc/resolv.conf`, points it at Cloudflare's resolvers, and makes it
   immutable (`chattr +i`), so a DHCP renewal cannot put your ISP's DNS back.

`warp off` reverses all of it and restores `resolv.conf`. `warp status` shows
`wg show wgcf`.

## Limits

- **Not a kill switch.** If the tunnel drops, traffic goes out directly. For
  a kill switch, add an nftables rule that drops output not to the endpoint
  and not via `wgcf`.
- **No split tunnelling**: everything goes through it.
- **Speed**: WARP's free tier was about as fast as no WARP here (about 200 Mbit/s
  on, 170 off). Your result depends on the nearest Cloudflare location.
- Cloudflare sees all your traffic instead of your ISP. That is the trade.

## Start it at boot

Add `warp on` to `/etc/rc.local`. It needs the network, so wait for a default
route first:

```sh
( for i in $(seq 30); do ip route show default | grep -q . && break; sleep 1; done
  warp on ) &
```
