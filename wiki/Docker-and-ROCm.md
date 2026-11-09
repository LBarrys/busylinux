# Docker, Podman and ROCm

## Docker

The kernel has what Docker needs: namespaces, cgroups v2 with BPF device
control, overlayfs, veth, bridges, NAT and the iptables compatibility layer.
The firewall already lets container bridges out and published ports in.

```sh
doas apk add docker
doas mkdir -p /etc/service/docker/log
printf '#!/bin/sh\nexec dockerd 2>&1\n' | doas tee /etc/service/docker/run > /dev/null
doas cp /etc/service/ntpd/log/run /etc/service/docker/log/
doas chmod 755 /etc/service/docker/run
```

runit starts it within five seconds. The `log/run` copied from `ntpd` sends
dockerd's output to syslog, tagged `docker`.

To run `docker` without `doas`, join its group and log in again:

```sh
doas addgroup alice docker
```

**Membership in `docker` amounts to root**: any member can start a
container that mounts `/` and changes anything.

## Podman

Rootless containers, no daemon, no group that equals root:

```sh
doas apk add podman
echo "alice:100000:65536" | doas tee -a /etc/subuid /etc/subgid
podman run --rm -it alpine
```

The firewall lets Podman's DNS (`aardvark-dns`) answer on `podman*` bridges.

## ROCm

The kernel has `HSA_AMD` and `HSA_AMD_SVM` for the compute device
`/dev/kfd`. mdevd needs a rule for its mode; add to `/etc/mdev.conf`:

```
kfd root:video 0660
```

then reboot or `doas mdevd-coldplug`. ROCm itself is glibc software. Run it
in a container (AMD's `rocm/*` images, with `--device /dev/kfd --device /dev/dri`)
or in the Arch tree (`arch-chroot pacman -S rocm-hip-runtime`).
The RX 7900 GRE is `gfx1100`, which ROCm supports.
