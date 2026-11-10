# CI and releases

Everything runs on GitHub Actions. There are two workflows.

## CI (`.github/workflows/ci.yml`)

On pushes to `main`, pull requests, and by hand.

### lint

- **shellcheck** at warning level on `tools/*`, the tests, the CI scripts, every
  recipe's `build`, and the shell scripts in `busylinux-init`.
- **nftables**: `nft -c -f` checks the ruleset parses.
- **Kernel signature**: runs `.github/kernel-bump.sh`, which checks that the
  newest kernel of the series is signed correctly (and would move the recipe to it).

### build

1. **Package cache**: Actions' cache of `cache/`, keyed by the Dockerfile,
   `tools/*.sh` and `pkgs/`, restoring the newest earlier one. `build.sh`
   rebuilds only recipes whose stamp changed.
2. **Prebuilt kernel**: the Actions cache does not cross branches, and the
   kernel is the slow part, so compiled kernels are also kept as assets of a
   release named `kernel-cache`. Each is keyed by a hash of exactly what makes
   the kernel: the Dockerfile's `FROM` and `RUN` lines, `lib.sh`, the recipe's
   files by content, and `VM_SUPPORT=1`. Moving files around does not
   recompile it.
3. **Build** in the container, with `VM_SUPPORT=1` for QEMU. A prebuilt kernel
   is signed again with this run's key (`apk adbsign`) and verified.
4. **Save the kernel** to `kernel-cache` when it was compiled, keeping the
   five newest. Not from forks: they have no write token.
5. **Boot test** (`tests/boot-test.sh`).
6. The **console log** is kept as an artifact, also on failure.

### The boot test

It installs `out/rootfs.tar.gz` with the real `install.sh` onto a 4 GB disk
image, boots it in QEMU with OVMF (UEFI) and Limine on the serial entry, logs
in on the serial console, and runs checks, each a shell command that must
succeed:

- the root found by partition name and checked by `fsck`
- the firewall's input and forward policies are `drop`; DHCP gets a lease
- the services run
- the ESP, cgroup2, `/dev/fd`, `/tmp` mounts
- `/dev/ntsync` and `/dev/uinput` modes
- sysctls: SysRq, `max_map_count`, io_uring off, `mmap_min_addr`
- no GMP or Readline; syslog works
- zram swap; the tty logins' realtime and file limits
- `rc.local` ran; mimalloc in services and the shell
- IPv6 privacy; Landlock and MGLRU are on
- sched_ext: off at boot; `scx_simple` starts through the `scx` service and stops again
- the packages are pinned
- **fail closed**: flushing the ruleset keeps DHCP down; reloading brings it back
- DHCP ignores bridges
- libudev-zero is this repository's; mdevd runs; a hotplugged USB sound card gets
  its module and node
- `update.sh` rebuilds two recipes in its build root, leaves no build tools,
  and reports the `.apk-new` it caused; `doctor` warns about it and nothing else
- the desktop packages install with every library found, and dwl starts headless
- the weekly update check runs
- **the power button**: rcK runs, a slow service gets its time to stop, the
  root file system is left clean, and services logged to syslog, not the console

## Kernel (`.github/workflows/kernel.yml`)

Every Monday, `kernel-bump.sh`:

1. asks kernel.org for the newest release of the recipe's series (6.18);
2. downloads it and its signature, and **checks it with `gpgv`** against Linus
   Torvalds' and Greg Kroah-Hartman's keys, fetched from kernel.org's WKD,
   accepting only their fingerprints;
3. updates `meta`, `sources` and `sha256sums`.

The workflow then opens a pull request on a `kernel-VERSION` branch and
starts CI on it. It first deletes the `kernel-*` branches whose pull requests
are merged or closed. A pull request opened with Actions' own token starts no
workflows itself. For this to work, allow Actions to create pull requests:
**Settings > Actions > General > Allow GitHub Actions to create and approve
pull requests**.

Review the pull request when CI is green, and merge it. Then
`doas tools/update.sh linux-busylinux` and reboot.

## Dependabot

Weekly pull requests for the builder's pinned `alpine:edge` digest and for
the Actions' versions.
