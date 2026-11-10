# Updating

There are two kinds of package, and each updates in its own way.

## Alpine's packages

```sh
doas apk upgrade
```

Nothing upgrades by itself. Once a week cron runs `updates-check`, which counts
the waiting updates. The count is shown at login:

```
12 package updates are waiting: doas apk upgrade
```

(`/usr/libexec/busylinux/updates-check` runs hourly but only does anything once
the last count is a week old. The notice lives in `/var/lib/busylinux/updates`.)

## This repository's packages

`linux-busylinux`, `busylinux-init`, `libudev-zero`, `nftables`, `dwl`,
`gammastep`, `pipemixer`, `cclip`, `wgcf`, `busylinux-fonts` and `busybox-runit` come from `pkgs/`. They are
**pinned**: `/etc/apk/world` names them as `NAME@busylinux`, and the
`@busylinux` repository is the local one at `/var/lib/busylinux/repo`. So
`apk upgrade` never swaps them for Alpine's builds of the same name. Some
also carry a `release` of 100 or more, so apk prefers them by version too.

To update them, pull this repository and rebuild them on the machine:

```sh
git pull
tools/update.sh                        # lists the recipes: version in pkgs/, version installed
doas tools/update.sh linux-busylinux   # one, e.g. the kernel
doas tools/update.sh --all             # all of them
```

`update.sh` asks before it builds (`--yes` skips that). Then it:

1. Makes or reuses a separate Alpine root, `/var/tmp/busylinux-update/root`,
   and builds the recipes inside it with `chroot`. Compilers and `-dev` packages
   go there, never onto the system, and cannot clash with what is installed.
   (The first time this failed without it: `libinput-dev` pulled in `eudev-dev`,
   which conflicts with libudev-zero.)
2. Signs the packages with `/root/keys/local.rsa`, made on first use, whose
   public half goes into `/etc/apk/keys`.
3. Adds them to `/var/lib/busylinux/repo` and installs them with `apk`. With
   `--all`, recipes you never installed are built but not installed.
4. Lists any `.apk-new` files (below).

When the recipe's version is the one installed, the new package gets the
next release number, so apk sees it as newer. A new kernel needs a reboot. `doctor` says so until you do.

## `.apk-new` files

When an update brings a new version of a file in `/etc` that you have edited,
apk keeps yours and puts the new one beside it as `FILE.apk-new`. Look at the
difference, merge what you want, and delete the `.apk-new`:

```sh
find /etc -name '*.apk-new'
diff /etc/nftables.conf /etc/nftables.conf.apk-new
```

## `doctor`

```sh
doas tools/doctor
```

A read-only check that prints one line per item and exits 1 if anything is
a `WARN`:

- the running kernel against the installed `linux-busylinux` (reboot pending?)
- each recipe's installed version against `pkgs/`, and whether it is pinned
- leftovers of interrupted builds or setups (`.busylinux-build`, `.warp-setup` in `/etc/apk/world`)
- `.apk-new` files
- every runit service is up, and mdevd runs
- services use mimalloc, zram swap is on, the firewall is loaded, `/` is under 90% full
- the weekly update count

It re-runs itself through `doas` when started as a user.

## The Arch tree

The Steam tree updates apart from all this:

```sh
arch-chroot update
```

See [Steam and games](Steam-and-Games.md#updating-the-tree).
